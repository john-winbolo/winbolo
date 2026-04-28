/*
 * imgui_game_viewport.cpp - Game background rendering and input handling
 *
 * Copyright (c) 2024
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "imgui_game_viewport.h"
#include "imgui_main_menu.h"
#include "imgui.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

/* Include logviewer.h (which pulls in backend.h with updateType enum and lv_ declarations).
 * Do NOT include draw.h by bare name — it resolves to src/gui/sdl3/draw.h which
 * pulls in bolo headers with conflicting type definitions. */
extern "C" {
    #include "logviewer.h"
    void lv_screenMouseInformationClick(int xPos, int yPos);
    void lv_screenMouseClick(int xPos, int yPos);
    void lv_drawDirtyScreen(void);
    struct SDL_Texture *lv_drawGetGameTexture(void);
    void lv_drawZoomIn(int mouseScreenX, int mouseScreenY);
    void lv_drawZoomOut(int mouseScreenX, int mouseScreenY);
    float lv_drawGetZoomLevel(void);
    void lv_windowPlay(void);
    void lv_windowPause(void);
    void lv_screenGetSubOffset(int *x, int *y);
    void lv_screenPanToTotalPixels(int totalPxX, int totalPxY);
}

static LogViewerState *s_lv = nullptr;

/* Tile size constant from tiles.h */
#define TILE_SIZE_X 16
#define TILE_SIZE_Y 16

/* Direction constants for lv_screenUpdate() - uses updateType enum from backend.h */

/* Drag threshold in pixels - must match main.c */
#define DRAG_THRESHOLD 4

/* Drag state. Pan position is tracked as a total in zoom-1 native pixels
 * so the drag handler can pan smoothly at sub-tile granularity instead of
 * snapping to whole tiles. */
static bool s_is_dragging = false;
static bool s_is_mouse_down = false;
static float s_drag_start_x = 0.0f;
static float s_drag_start_y = 0.0f;
static int s_drag_start_total_px_x = 0;
static int s_drag_start_total_px_y = 0;


void lv_imgui_game_viewport_init(struct LogViewerState *lv) {
    s_lv = lv;
    s_is_dragging = false;
    s_is_mouse_down = false;
    s_drag_start_x = 0.0f;
    s_drag_start_y = 0.0f;
    s_drag_start_total_px_x = 0;
    s_drag_start_total_px_y = 0;
}

/* Read the current pan position as a single total in zoom-1 native pixels. */
static void get_total_pan_pixels(int *outX, int *outY) {
    unsigned char ox = 0, oy = 0;
    int sx = 0, sy = 0;
    lv_screenGetOffsets(&ox, &oy);
    lv_screenGetSubOffset(&sx, &sy);
    *outX = (int)ox * TILE_SIZE_X + sx;
    *outY = (int)oy * TILE_SIZE_Y + sy;
}

void lv_imgui_game_viewport_render_background(void) {
    /* This function is now a no-op - the game texture is rendered
     * in the main loop via lv_drawBlitGameTexture() which already
     * handles the menu bar offset.
     * 
     * The input processing is handled separately in 
     * lv_imgui_game_viewport_process_input() which is called after
     * ImGui renders to check if input should go to the game.
     */
}

/* Convert screen coordinates to game texture coordinates.
 * Accounts for menu bar offset at the top of the window and the user
 * zoom level (the texture target is rendered at native size and then
 * scaled by zoom at blit time, so a screen pixel maps to 1/zoom
 * texture pixels).
 * Returns 1 if coordinates are within game texture bounds, 0 otherwise.
 */
static int screen_to_game_coords(float screen_x, float screen_y,
                                  int *game_x, int *game_y) {
    float menu_bar_height = lv_imgui_get_menu_bar_height();
    float zoom = lv_drawGetZoomLevel();
    if (zoom <= 0.0f) zoom = 1.0f;

    /* Subtract menu bar offset, then divide by zoom to get texture coords. */
    *game_x = (int)(screen_x / zoom);
    *game_y = (int)((screen_y - menu_bar_height) / zoom);

    /* Check if coordinates are within game texture bounds */
    if (*game_x < 0 || *game_y < 0) {
        return 0;
    }

    /* Game texture dimensions */
    int game_width = 100 * TILE_SIZE_X;  /* Max map size */
    int game_height = 100 * TILE_SIZE_Y;

    if (*game_x >= game_width || *game_y >= game_height) {
        return 0;
    }

    return 1;
}

int lv_imgui_game_viewport_process_input(void) {
    ImGuiIO& io = ImGui::GetIO();
    int input_processed = 0;
    
    /* Only process input if a log is loaded */
    if (s_lv->isLoaded == 0) {
        return 0;
    }
    
    /* Check if ImGui wants to capture input - if so, don't process for game */
    if (io.WantCaptureMouse) {
        /* ImGui UI is using the mouse - reset drag state */
        if (s_is_mouse_down && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            s_is_mouse_down = false;
            s_is_dragging = false;
        }
        return 0;
    }

    /* Mouse-wheel zoom is handled directly in the main SDL event loop
     * in logviewer.c (gated on lv_imgui_want_capture_mouse), so it works
     * regardless of whether a log is loaded yet and doesn't depend on
     * io.MouseWheel surviving across ImGui frame boundaries. */

    /* Process mouse input */
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        if (!s_is_mouse_down) {
            /* Mouse just went down - start tracking */
            s_is_mouse_down = true;
            s_is_dragging = false;
            s_drag_start_x = io.MousePos.x;
            s_drag_start_y = io.MousePos.y;
            get_total_pan_pixels(&s_drag_start_total_px_x,
                                 &s_drag_start_total_px_y);
        } else {
            /* Mouse is still down - check for drag */
            float dx = io.MousePos.x - s_drag_start_x;
            float dy = io.MousePos.y - s_drag_start_y;

            /* Check if we've exceeded the drag threshold */
            if (!s_is_dragging) {
                if (dx >= DRAG_THRESHOLD || dx <= -DRAG_THRESHOLD ||
                    dy >= DRAG_THRESHOLD || dy <= -DRAG_THRESHOLD) {
                    s_is_dragging = true;
                }
            }

            /* Pan in zoom-1 native pixel units: each on-screen pixel maps
             * to 1/zoom texture pixels. Drag right => see further left of
             * map, so subtract the delta. */
            if (s_is_dragging) {
                float zoom = lv_drawGetZoomLevel();
                if (zoom <= 0.0f) zoom = 1.0f;
                int delta_px_x = (int)(dx / zoom);
                int delta_px_y = (int)(dy / zoom);
                int new_total_x = s_drag_start_total_px_x - delta_px_x;
                int new_total_y = s_drag_start_total_px_y - delta_px_y;

                lv_drawDirtyScreen();  /* Invalidate cache when view scrolls */
                lv_screenPanToTotalPixels(new_total_x, new_total_y);
                input_processed = 1;
            }
        }
    }

    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        if (s_is_mouse_down) {
            if (s_is_dragging) {
                /* End of drag - finalize position (sub-pixel-aware, see above) */
                float dx = io.MousePos.x - s_drag_start_x;
                float dy = io.MousePos.y - s_drag_start_y;
                float zoom = lv_drawGetZoomLevel();
                if (zoom <= 0.0f) zoom = 1.0f;
                int delta_px_x = (int)(dx / zoom);
                int delta_px_y = (int)(dy / zoom);
                int new_total_x = s_drag_start_total_px_x - delta_px_x;
                int new_total_y = s_drag_start_total_px_y - delta_px_y;

                lv_drawDirtyScreen();  /* Invalidate cache when view scrolls */
                lv_screenPanToTotalPixels(new_total_x, new_total_y);
            } else {
                /* Click without drag - handle as selection */
                int game_x, game_y;
                if (screen_to_game_coords(io.MousePos.x, io.MousePos.y, &game_x, &game_y)) {
                    /* Check mode to determine which click function to use */
                    if (lv_imgui_get_mode_information()) {
                        lv_screenMouseInformationClick(game_x, game_y);
                    } else {
                        lv_screenMouseClick(game_x, game_y);
                    }
                    input_processed = 1;
                }
            }
            
            s_is_mouse_down = false;
            s_is_dragging = false;
        }
    }
    
    /* Right-click for centering on point */
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        if (!io.WantCaptureMouse) {
            int game_x, game_y;
            if (screen_to_game_coords(io.MousePos.x, io.MousePos.y, &game_x, &game_y)) {
                lv_drawDirtyScreen();  /* Invalidate cache when view offset changes */
                lv_screenMouseCentreClick(game_x, game_y);
                input_processed = 1;
            }
        }
    }
    
    /* Keyboard input - arrow keys for scrolling */
    if (!io.WantCaptureKeyboard) {
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
            lv_drawDirtyScreen();  /* Invalidate cache when view scrolls */
            lv_screenUpdate(left);
            input_processed = 1;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
            lv_drawDirtyScreen();
            lv_screenUpdate(right);
            input_processed = 1;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
            lv_drawDirtyScreen();
            lv_screenUpdate(up);
            input_processed = 1;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
            lv_drawDirtyScreen();
            lv_screenUpdate(down);
            input_processed = 1;
        }
        /* Spacebar toggles playback. Matches the Play/Pause button in the
         * Controls panel; same isLoaded gating since the function returns
         * early when no log is loaded. */
        if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
            if (s_lv->playIsPlaying) {
                lv_windowPause();
            } else {
                lv_windowPlay();
            }
            input_processed = 1;
        }
    }

    return input_processed;
}