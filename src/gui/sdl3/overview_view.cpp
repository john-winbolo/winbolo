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
 *                OverviewMap, then the tanks, men and shells
 *                standing on the squares it says are visible;
 *                the camera maths it sits on lives in
 *                overview_camera.cpp.
 *********************************************************/

#include <SDL3/SDL.h>

#include "imgui.h"

#include "overview_view.h"
#include "key_claims.h"     /* keyIsClaimedByGame */
#include "build_cursor.h"   /* buildCursorSetTile */

extern "C" {
#include "global.h"
#include "client_sim.h"     /* clientSimGetOverviewMap, clientSimGetMyTankMapPos,
                               clientSimIsMyTankAlive,
                               clientSimPrepareOverviewEntities,
                               clientSimManMoveToMap,
                               clientSimGetCurrentBuildSelect */
#include "../clientmutex.h" /* the build dispatch runs on the sim's data */
#include "cursor.h"         /* cursorSetCursor — the game's crosshair pointer */
#include "overview_types.h"
#include "screentank.h"
#include "screenlgm.h"
#include "screenbullet.h"
#include "../tiles.h"       /* MINE_X / MINE_Y, TILE_SIZE_X / TILE_SIZE_Y */
#include "sprite_positions.h"
#include "mapview.h"        /* mapViewDrawShells / Tanks / LGMs */
}

/* Colour mod for a remembered-but-not-currently-visible square. Dark enough
 * to read as "this is memory, not sight" at a glance, light enough that the
 * terrain type is still identifiable. */
#define OVERVIEW_FROZEN_MOD 110

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

struct OverviewView {
    OverviewCamera cam;

    /* The offscreen the host blits. Textures belong to the renderer that
     * made them, so the renderer is tracked alongside the size and all
     * three are what a reallocation tests. */
    SDL_Texture   *target;
    SDL_Renderer  *targetRenderer;
    int            targetW;
    int            targetH;

    /* The OS pointer is switched to the game's crosshair while it is over the
     * map, so the view has to remember that it did the switching — nothing
     * else will put the system cursor back. dragWasActive carries the drag
     * one frame further, for the reason in overviewViewHandleInput. */
    bool           crosshairOn;
    bool           dragWasActive;
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

/* One colour-mod run over the visible squares: livePass draws the squares
 * inside a live region at full brightness, the other pass draws the
 * remembered ones dimmed. Splitting them this way sets the mod twice per
 * frame instead of once per square, which keeps the renderer batching. */
static void overviewViewDrawPass(SDL_Renderer *r, SDL_Texture *tiles, int ss,
                                 const OverviewCamera *cam, int viewW, int viewH,
                                 const OverviewMap *om,
                                 int left, int top, int right, int bottom,
                                 bool livePass) {
    if (livePass) {
        SDL_SetTextureColorMod(tiles, 255, 255, 255);
    } else {
        SDL_SetTextureColorMod(tiles, OVERVIEW_FROZEN_MOD,
                               OVERVIEW_FROZEN_MOD, OVERVIEW_FROZEN_MOD);
    }

    float tilePx = (float)OVERVIEW_TILE_PX * overviewCameraZoomScale(cam);
    float cellW  = (float)(TILE_SIZE_X * ss);
    float cellH  = (float)(TILE_SIZE_Y * ss);
    SDL_FRect mineSrc = { (float)(MINE_X * ss), (float)(MINE_Y * ss),
                          cellW, cellH };

    /* x outer, y inner: the memory is [x][y], so the inner walk is
     * contiguous. */
    for (int mx = left; mx <= right; mx++) {
        for (int my = top; my <= bottom; my++) {
            BYTE tile = om->tile[mx][my];
            if (tile == OVERVIEW_UNSEEN) continue;  /* the black clear shows */

            BYTE flags = om->flags[mx][my];
            bool isLive = (flags & OVERVIEW_F_LIVE) != 0;
            if (isLive != livePass) continue;

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
            SDL_FRect src  = { (float)(mapViewPosX[tile] * ss),
                               (float)(mapViewPosY[tile] * ss),
                               cellW, cellH };
            SDL_RenderTexture(r, tiles, &src, &dest);

            if ((flags & OVERVIEW_F_MINE) != 0) {
                SDL_RenderTexture(r, tiles, &mineSrc, &dest);
            }
        }
    }
}

/* The name screenTanksPrepare built, cut down to what the overview shows.
 * The label carries a trailing "@<location>" in long-label mode, which the
 * main view splits off to hang a flag or a brain icon beside the name; there
 * is no room for either here, so the name alone is drawn. */
static void overviewViewShortLabel(const char *label, char *out, size_t outLen) {
    SDL_strlcpy(out, label, outLen);
    char *at = SDL_strrchr(out, '@');
    if (at != NULL) *at = '\0';
}

/* Copies the entries the player is allowed to see into a second set of lists.
 * The builders work over the whole map — wider than anything the server culls
 * to — so this is where the fog is applied: an entity is kept only when the
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

/* Tank names beside the sprites, in the renderer's own 8x8 debug font — the
 * pop-out has its own renderer, and the TTF faces the main window's label
 * pass uses are bound to that window's. Same choice bg_game.c makes for its
 * map-name caption. */
static void overviewViewDrawLabels(SDL_Renderer *r, const OverviewCamera *cam,
                                   int viewW, int viewH,
                                   const screenTanks *tks) {
    BYTE total = screenTanksGetNumEntries(tks);
    BYTE count;

    for (count = 1; count <= total; count++) {
        BYTE mx, my, px, py, frame, playerNum;
        char name[PLAYER_NAME_LEN];
        char shown[PLAYER_NAME_LEN];
        float sx = 0.0f, sy = 0.0f;

        screenTanksGetItem(tks, count, &mx, &my, &px, &py, &frame, &playerNum,
                           name);
        if (name[0] == '\0') continue;
        overviewViewShortLabel(name, shown, sizeof(shown));
        if (shown[0] == '\0') continue;

        /* One square to the right of the tank, as the main view places it. */
        overviewCameraWorldToScreen(cam, viewW, viewH,
                                    (float)mx + (float)px / (float)TILE_SIZE_X + 1.0f,
                                    (float)my + (float)py / (float)TILE_SIZE_Y,
                                    &sx, &sy);
        if (sx > (float)viewW || sy > (float)viewH || sy < 0.0f) continue;
        if (sx < 0.0f) sx = 0.0f;

        /* Terrain runs from black sea to pale road under the same label, so
         * the name is drawn over its own shadow rather than trusting one
         * colour to read against all of it. */
        SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
        SDL_RenderDebugText(r, sx + 1.0f, sy + 1.0f, shown);
        SDL_SetRenderDrawColor(r, 230, 230, 230, 255);
        SDL_RenderDebugText(r, sx, sy, shown);
    }
}

/* The sprite overlay: everything that moves, on the squares the player can
 * see this instant. Runs on the offscreen the terrain passes just filled. */
static void overviewViewDrawEntities(SDL_Renderer *r, SDL_Texture *tiles, int ss,
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
     * draws. Solid while cursor mode is on; a target that is only locked in
     * shows faint, which is the split the main view makes too. Ahead of the
     * sprite passes so tanks and men stand on top of it. */
    BYTE bcX = 0, bcY = 0;
    bool cursorSolid = buildCursorGetTile(&bcX, &bcY);
    if (cursorSolid || buildCursorGetTargetTile(&bcX, &bcY)) {
        int bbx = (int)bcX * TILE_SIZE_X;
        int bby = (int)bcY * TILE_SIZE_Y;
        SDL_FRect src = {
            (float)(MOUSE_SQUARE_X * ss), (float)(MOUSE_SQUARE_Y * ss),
            (float)(TILE_SIZE_X * ss),    (float)(TILE_SIZE_Y * ss)
        };
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
        overviewViewDrawLabels(r, cam, viewW, viewH, &tks);
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
                                            int w, int h, ClientSim *cs) {
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
         * player is, so follow mode holds the centre it already had. */
        BYTE tankX = 0, tankY = 0;
        if (clientSimIsMyTankAlive(cs) &&
            clientSimGetMyTankMapPos(cs, &tankX, &tankY)) {
            overviewCameraFollowTick(&v->cam, w, h, tankX, tankY);
        }

        int left = 0, top = 0, right = 0, bottom = 0;
        if (overviewCameraVisibleRange(&v->cam, w, h,
                                       &left, &top, &right, &bottom)) {
            overviewViewDrawPass(r, tiles, sheetScale, &v->cam, w, h, om,
                                 left, top, right, bottom, true);
            overviewViewDrawPass(r, tiles, sheetScale, &v->cam, w, h, om,
                                 left, top, right, bottom, false);
        }
        /* Hand the host's texture back the way we found it — the sprite pass
         * below draws from it too, and at full brightness. */
        SDL_SetTextureColorMod(tiles, 255, 255, 255);

        overviewViewDrawEntities(r, tiles, sheetScale, crosshair, &v->cam,
                                 w, h, om, cs);
    }

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
                                        const keyItems *keys) {
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

    if (hovered && io.MouseWheel != 0.0f) {
        ImVec2 rectMin = ImGui::GetItemRectMin();
        ImVec2 mouse   = ImGui::GetMousePos();
        int steps = (io.MouseWheel > 0.0f) ? 1 : -1;
        overviewCameraZoomAt(cam, viewW, viewH,
                             mouse.x - rectMin.x, mouse.y - rectMin.y, steps);
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

    /* Arrows are the keyboard's pan, and panning is what follow mode does
     * for you — while it is on they would fight it for one frame and lose. */
    if (!cam->follow) {
        float dx = 0.0f, dy = 0.0f;
        if (overviewKeyDown(keys, ImGuiKey_LeftArrow))  dx -= OVERVIEW_ARROW_STEP_PX;
        if (overviewKeyDown(keys, ImGuiKey_RightArrow)) dx += OVERVIEW_ARROW_STEP_PX;
        if (overviewKeyDown(keys, ImGuiKey_UpArrow))    dy -= OVERVIEW_ARROW_STEP_PX;
        if (overviewKeyDown(keys, ImGuiKey_DownArrow))  dy += OVERVIEW_ARROW_STEP_PX;
        if (dx != 0.0f || dy != 0.0f) {
            overviewCameraPan(cam, viewW, viewH, dx, dy);
        }
    }

    /* Nothing to centre on while the tank is dead, so the key does nothing
     * rather than throwing the view at wherever the corpse reads. */
    if (overviewKeyPressed(keys, ImGuiKey_Home) ||
        overviewKeyPressed(keys, ImGuiKey_C)) {
        BYTE tankX = 0, tankY = 0;
        if (clientSimIsMyTankAlive(cs) &&
            clientSimGetMyTankMapPos(cs, &tankX, &tankY)) {
            overviewCameraCenterOnTank(cam, viewW, viewH, tankX, tankY);
        }
    }
}
