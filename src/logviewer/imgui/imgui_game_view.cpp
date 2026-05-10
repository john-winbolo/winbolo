/*
 * imgui_game_view.cpp - Game-UI skin for the Log Viewer (trailer capture).
 *
 * Copyright (c) 2024
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "imgui_game_view.h"
#include "imgui.h"
#include "../../gui/lang.h"

extern "C" {
    /* logviewer.h transitively includes logviewer/players.h (resolved
     * via same-directory-first lookup). A direct #include "players.h"
     * here would resolve to bolo/players.h instead — -Isrc/bolo
     * precedes -Isrc/logviewer on the WinBolo target's include path —
     * and the two header files declare conflicting `players` /
     * `player` typedefs. So we deliberately rely on the transitive
     * include and don't pull players.h in directly. */
    #include "logviewer.h"
    #include "../game_view.h"
}

/* Game-view viewport size in tiles (must match GV_SCREEN_TILES in
 * game_view.c — both files set it to 16; duplicated here so this TU
 * doesn't pull in game_view.c's bolo-typed headers). */
static const int GV_SCREEN_TILES = 16;

/* Tile width in unzoomed pixels — must match TILE_SIZE_X (16). The
 * camera works in unzoomed pixel units; lv_screenPanToTotalPixels
 * decomposes a total pan into (xOffset,yOffset) tiles + (subPxX,subPxY)
 * sub-tile pixels. */
static const int GV_TILE_PX = 16;
static const int GV_VIEWPORT_PX = GV_SCREEN_TILES * GV_TILE_PX;  /* 256 */

/* Dead-zone scroll margin: the inner 8×8 region of the 16×16 viewport is
 * the "safe zone"; the tank must leave it before the camera nudges. */
static const int GV_SCROLL_MARGIN_TILES = 4;
static const int GV_SCROLL_MARGIN_PX = GV_SCROLL_MARGIN_TILES * GV_TILE_PX;  /* 64 */

/* Easing for the dead-zone follow. Each frame the camera moves
 * `delta/GV_EASE_DIVISOR` pixels toward the target, capped at
 * GV_MAX_STEP_PX so a teleport (e.g. fast-forward catch-up) doesn't
 * snap visibly. The min-step-of-1 floor keeps the camera converging
 * on the final pixel instead of stalling near the target. */
static const int GV_EASE_DIVISOR = 4;
static const int GV_MAX_STEP_PX = 16;

/* One-shot: centre the viewport on the camera tank's pixel-precise
 * world position, clamped to map bounds. Called from logviewer.c on
 * game-view enter and on Tab cycle (where the new target may be far
 * outside the current viewport). Frame-to-frame tracking is the
 * eased dead-zone update below. */
void lv_imgui_game_view_init_camera(LogViewerState *lv) {
    if (lv == nullptr || !lv->isLoaded) {
        return;
    }
    if (!lv_playersIsInUse(lv->cameraSlot)) {
        return;
    }
    /* Dead tanks have their map coords zeroed by lv_playersUpdateTank in
     * the death handlers; centring on (0,0) would jump the camera to
     * the map corner. Freeze the viewport where the tank died until it
     * respawns (gameViewHud[slot].alive flips back true). The death-
     * static overlay in lv_drawGameViewFrame paints over the frozen map
     * for the duration. */
    if (lv->cameraSlot < MAX_TANKS && !lv->gameViewHud[lv->cameraSlot].alive) {
        return;
    }

    BYTE mx, my, px, py, frame;
    bool onBoat;
    lv_playersGetTankDetails(lv->cameraSlot, &mx, &my, &px, &py, &frame, &onBoat);

    int tankWorldPxX = (int)mx * GV_TILE_PX + (int)px;
    int tankWorldPxY = (int)my * GV_TILE_PX + (int)py;
    int targetPxX = tankWorldPxX - GV_VIEWPORT_PX / 2;
    int targetPxY = tankWorldPxY - GV_VIEWPORT_PX / 2;

    /* lv_screenPanToTotalPixels handles map-bound clamping and the
     * (xOffset,yOffset)/(subPxX,subPxY) decomposition. */
    lv_screenPanToTotalPixels(targetPxX, targetPxY);
    lv->wantScreenUpdate = TRUE;
}

/* Pixel-precise dead-zone autoscroll. Tank screen-pixel position inside
 * the inner 128×128 safe zone — camera sits still, tank slides on
 * screen. Tank outside — ease the camera toward a target that puts the
 * tank just inside the safe-zone boundary on that side. Final hard
 * re-clamp guarantees the tank never sits fully off-screen even if
 * easing falls behind a teleport. */
void lv_imgui_game_view_update_camera(LogViewerState *lv) {
    if (lv == nullptr || !lv->isLoaded) {
        return;
    }
    if (!lv_playersIsInUse(lv->cameraSlot)) {
        return;
    }
    /* Same alive guard as init_camera: dead tanks' map coords are zeroed,
     * which would pull the camera to (0,0). Freeze here until respawn. */
    if (lv->cameraSlot < MAX_TANKS && !lv->gameViewHud[lv->cameraSlot].alive) {
        return;
    }

    BYTE mx, my, px, py, frame;
    bool onBoat;
    lv_playersGetTankDetails(lv->cameraSlot, &mx, &my, &px, &py, &frame, &onBoat);

    int tankWorldPxX = (int)mx * GV_TILE_PX + (int)px;
    int tankWorldPxY = (int)my * GV_TILE_PX + (int)py;
    int currentPxX = (int)lv->xOffset * GV_TILE_PX + lv->subPxX;
    int currentPxY = (int)lv->yOffset * GV_TILE_PX + lv->subPxY;

    int tankScreenPxX = tankWorldPxX - currentPxX;
    int tankScreenPxY = tankWorldPxY - currentPxY;

    int targetPxX = currentPxX;
    int targetPxY = currentPxY;

    if (tankScreenPxX < GV_SCROLL_MARGIN_PX) {
        targetPxX = tankWorldPxX - GV_SCROLL_MARGIN_PX;
    } else if (tankScreenPxX >= GV_VIEWPORT_PX - GV_SCROLL_MARGIN_PX) {
        targetPxX = tankWorldPxX - (GV_VIEWPORT_PX - GV_SCROLL_MARGIN_PX);
    }
    if (tankScreenPxY < GV_SCROLL_MARGIN_PX) {
        targetPxY = tankWorldPxY - GV_SCROLL_MARGIN_PX;
    } else if (tankScreenPxY >= GV_VIEWPORT_PX - GV_SCROLL_MARGIN_PX) {
        targetPxY = tankWorldPxY - (GV_VIEWPORT_PX - GV_SCROLL_MARGIN_PX);
    }

    int dx = targetPxX - currentPxX;
    int dy = targetPxY - currentPxY;
    int stepX = dx / GV_EASE_DIVISOR;
    int stepY = dy / GV_EASE_DIVISOR;
    if (stepX == 0 && dx != 0) stepX = (dx > 0) ?  1 : -1;
    if (stepY == 0 && dy != 0) stepY = (dy > 0) ?  1 : -1;
    if (stepX >  GV_MAX_STEP_PX) stepX =  GV_MAX_STEP_PX;
    if (stepX < -GV_MAX_STEP_PX) stepX = -GV_MAX_STEP_PX;
    if (stepY >  GV_MAX_STEP_PX) stepY =  GV_MAX_STEP_PX;
    if (stepY < -GV_MAX_STEP_PX) stepY = -GV_MAX_STEP_PX;

    int newPxX = currentPxX + stepX;
    int newPxY = currentPxY + stepY;

    /* Hard re-clamp: tank must never sit fully off-screen. Catches the
     * case where easing can't keep up with a teleport (e.g. cameraSlot
     * change without going through init_camera, or extreme fast-forward). */
    int afterScreenX = tankWorldPxX - newPxX;
    int afterScreenY = tankWorldPxY - newPxY;
    if (afterScreenX < 0)                    newPxX = tankWorldPxX;
    else if (afterScreenX >= GV_VIEWPORT_PX) newPxX = tankWorldPxX - GV_VIEWPORT_PX + 1;
    if (afterScreenY < 0)                    newPxY = tankWorldPxY;
    else if (afterScreenY >= GV_VIEWPORT_PX) newPxY = tankWorldPxY - GV_VIEWPORT_PX + 1;

    if (newPxX != currentPxX || newPxY != currentPxY) {
        lv_screenPanToTotalPixels(newPxX, newPxY);
        lv->wantScreenUpdate = TRUE;
    }
}

/* Phase A: body intentionally empty. Pixel-identical SDL render lands
 * in Phase D — see plans/ctrailer.md. */
void lv_imgui_render_game_view(LogViewerState *lv) {
    (void)lv;
}

/* Decorative menu bar matching the live game's six top-level menus.
 * Submenu bodies are intentionally empty for trailer footage. */
void lv_imgui_render_game_menu_bar(LogViewerState *lv) {
    if (lv == nullptr) return;

    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu(langGetText(STR_MENU_FILE))) {
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(langGetText(STR_MENU_EDIT))) {
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(langGetText(STR_MENU_WINBOLO))) {
            /* Phase E: zoom selector. Mirrors the SDLK_1..4 hotkeys in
             * logviewer.c. ASCII labels — the trailer build doesn't
             * extend lang.h with new STR_* entries. */
            int currentZoom = lv_drawGameViewGetZoom();
            if (ImGui::MenuItem("Zoom 1x", "1", currentZoom == 1)) {
                if (currentZoom != 1) {
                    lv_drawGameViewTeardown();
                    lv_drawGameViewSetup(1);
                    lv->wantScreenUpdate = TRUE;
                }
            }
            if (ImGui::MenuItem("Zoom 2x", "2", currentZoom == 2)) {
                if (currentZoom != 2) {
                    lv_drawGameViewTeardown();
                    lv_drawGameViewSetup(2);
                    lv->wantScreenUpdate = TRUE;
                }
            }
            if (ImGui::MenuItem("Zoom 3x", "3", currentZoom == 3)) {
                if (currentZoom != 3) {
                    lv_drawGameViewTeardown();
                    lv_drawGameViewSetup(3);
                    lv->wantScreenUpdate = TRUE;
                }
            }
            if (ImGui::MenuItem("Zoom 4x", "4", currentZoom == 4)) {
                if (currentZoom != 4) {
                    lv_drawGameViewTeardown();
                    lv_drawGameViewSetup(4);
                    lv->wantScreenUpdate = TRUE;
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(langGetText(STR_MENU_PLAYERS))) {
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(langGetText(STR_MENU_BRAINS))) {
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(langGetText(STR_MENU_HELP))) {
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }
}
