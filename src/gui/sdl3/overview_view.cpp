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
 *                Terrain and mines only, straight from the
 *                client's OverviewMap; the camera maths it
 *                sits on lives in overview_camera.cpp.
 *********************************************************/

#include <SDL3/SDL.h>

#include "imgui.h"

#include "overview_view.h"

extern "C" {
#include "global.h"
#include "client_sim.h"     /* clientSimGetOverviewMap, clientSimGetMyTankMapPos,
                               clientSimIsMyTankAlive */
#include "overview_types.h"
#include "../tiles.h"       /* MINE_X / MINE_Y, TILE_SIZE_X / TILE_SIZE_Y */
#include "sprite_positions.h"
}

/* Colour mod for a remembered-but-not-currently-visible square. Dark enough
 * to read as "this is memory, not sight" at a glance, light enough that the
 * terrain type is still identifiable. */
#define OVERVIEW_FROZEN_MOD 110

/* View pixels an arrow key moves the centre per frame. In pixels rather than
 * squares so the map slides at the same apparent speed at every zoom. */
#define OVERVIEW_ARROW_STEP_PX 12.0f

struct OverviewView {
    OverviewCamera cam;

    /* The offscreen the host blits. Textures belong to the renderer that
     * made them, so the renderer is tracked alongside the size and all
     * three are what a reallocation tests. */
    SDL_Texture   *target;
    SDL_Renderer  *targetRenderer;
    int            targetW;
    int            targetH;
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
        /* Hand the host's texture back the way we found it. */
        SDL_SetTextureColorMod(tiles, 255, 255, 255);
    }

    SDL_SetRenderTarget(r, NULL);
}

extern "C" void overviewViewHandleInput(OverviewView *v, bool hovered,
                                        int viewW, int viewH, ClientSim *cs) {
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

    if (hovered && io.MouseWheel != 0.0f) {
        ImVec2 rectMin = ImGui::GetItemRectMin();
        ImVec2 mouse   = ImGui::GetMousePos();
        int steps = (io.MouseWheel > 0.0f) ? 1 : -1;
        overviewCameraZoomAt(cam, viewW, viewH,
                             mouse.x - rectMin.x, mouse.y - rectMin.y, steps);
    }

    /* Keyboard zoom has no cursor to hold onto, so it anchors the centre. */
    float midX = (float)viewW * 0.5f;
    float midY = (float)viewH * 0.5f;
    if (ImGui::IsKeyPressed(ImGuiKey_Equal) ||
        ImGui::IsKeyPressed(ImGuiKey_KeypadAdd)) {
        overviewCameraZoomAt(cam, viewW, viewH, midX, midY, 1);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Minus) ||
        ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract)) {
        overviewCameraZoomAt(cam, viewW, viewH, midX, midY, -1);
    }

    /* Arrows are the keyboard's pan, and panning is what follow mode does
     * for you — while it is on they would fight it for one frame and lose. */
    if (!cam->follow) {
        float dx = 0.0f, dy = 0.0f;
        if (ImGui::IsKeyDown(ImGuiKey_LeftArrow))  dx -= OVERVIEW_ARROW_STEP_PX;
        if (ImGui::IsKeyDown(ImGuiKey_RightArrow)) dx += OVERVIEW_ARROW_STEP_PX;
        if (ImGui::IsKeyDown(ImGuiKey_UpArrow))    dy -= OVERVIEW_ARROW_STEP_PX;
        if (ImGui::IsKeyDown(ImGuiKey_DownArrow))  dy += OVERVIEW_ARROW_STEP_PX;
        if (dx != 0.0f || dy != 0.0f) {
            overviewCameraPan(cam, viewW, viewH, dx, dy);
        }
    }

    /* Nothing to centre on while the tank is dead, so the key does nothing
     * rather than throwing the view at wherever the corpse reads. */
    if (ImGui::IsKeyPressed(ImGuiKey_Home) || ImGui::IsKeyPressed(ImGuiKey_C)) {
        BYTE tankX = 0, tankY = 0;
        if (clientSimIsMyTankAlive(cs) &&
            clientSimGetMyTankMapPos(cs, &tankX, &tankY)) {
            overviewCameraCenterOnTank(cam, viewW, viewH, tankX, tankY);
        }
    }
}
