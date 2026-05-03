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

/* Dead-zone scroll margin: the inner 8×8 region of the 16×16 viewport is
 * the "safe zone"; the tank must leave it before the camera nudges. */
static const int GV_SCROLL_MARGIN = 4;

/* One-shot: centre xOffset/yOffset on the camera tank, clamped to map
 * bounds. Called from logviewer.c on game-view enter and on Tab cycle.
 * Frame-to-frame tracking is the dead-zone update below — this is for
 * cases where the new target may be far outside the current viewport. */
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

    int targetTileX = (int)mx - GV_SCREEN_TILES / 2;
    int targetTileY = (int)my - GV_SCREEN_TILES / 2;

    if (targetTileX < 0) targetTileX = 0;
    if (targetTileY < 0) targetTileY = 0;
    if (targetTileX > 256 - GV_SCREEN_TILES) targetTileX = 256 - GV_SCREEN_TILES;
    if (targetTileY > 256 - GV_SCREEN_TILES) targetTileY = 256 - GV_SCREEN_TILES;

    lv->xOffset = (BYTE)targetTileX;
    lv->yOffset = (BYTE)targetTileY;
    lv->wantScreenUpdate = TRUE;
}

/* Dead-zone autoscroll. Tank inside the inner 8×8 safe zone — leave the
 * camera alone (so manual arrow-key scroll is preserved). Tank outside —
 * nudge xOffset/yOffset by 1 tile toward the tank, then clamp to map
 * bounds and re-clamp so the tank can never sit fully off-screen. */
void lv_imgui_game_view_update_camera(LogViewerState *lv) {
    if (lv == nullptr || !lv->isLoaded) {
        return;
    }
    if (!lv_playersIsInUse(lv->cameraSlot)) {
        return;
    }
    /* Same alive guard as init_camera: dead tanks' map coords are zeroed,
     * which would pull the camera to (0,0) one tile per frame. Freeze
     * here until respawn. */
    if (lv->cameraSlot < MAX_TANKS && !lv->gameViewHud[lv->cameraSlot].alive) {
        return;
    }

    BYTE mx, my, px, py, frame;
    bool onBoat;
    lv_playersGetTankDetails(lv->cameraSlot, &mx, &my, &px, &py, &frame, &onBoat);

    int xOff = (int)lv->xOffset;
    int yOff = (int)lv->yOffset;

    if      ((int)mx <  xOff + GV_SCROLL_MARGIN)                       xOff--;
    else if ((int)mx >= xOff + GV_SCREEN_TILES - GV_SCROLL_MARGIN)     xOff++;
    if      ((int)my <  yOff + GV_SCROLL_MARGIN)                       yOff--;
    else if ((int)my >= yOff + GV_SCREEN_TILES - GV_SCROLL_MARGIN)     yOff++;

    if (xOff < 0) xOff = 0;
    if (yOff < 0) yOff = 0;
    if (xOff > 256 - GV_SCREEN_TILES) xOff = 256 - GV_SCREEN_TILES;
    if (yOff > 256 - GV_SCREEN_TILES) yOff = 256 - GV_SCREEN_TILES;

    if      ((int)mx <  xOff)                    xOff = (int)mx;
    else if ((int)mx >= xOff + GV_SCREEN_TILES)  xOff = (int)mx - GV_SCREEN_TILES + 1;
    if      ((int)my <  yOff)                    yOff = (int)my;
    else if ((int)my >= yOff + GV_SCREEN_TILES)  yOff = (int)my - GV_SCREEN_TILES + 1;

    if (xOff != (int)lv->xOffset || yOff != (int)lv->yOffset) {
        lv->xOffset = (BYTE)xOff;
        lv->yOffset = (BYTE)yOff;
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
