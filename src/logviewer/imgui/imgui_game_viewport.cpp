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

/* External functions from main.c and backend - using C types directly.
 * NOTE: We do NOT include global.h or backend.h because they have
 * typedef BYTE bool which conflicts with C++ built-in bool type. */
extern "C" {
    void screenUpdate(int direction);
    void screenPanToOffsets(unsigned char newXOffset, unsigned char newYOffset);
    void screenGetOffsets(unsigned char *x, unsigned char *y);
    void screenMouseCentreClick(int xPos, int yPos);
    void screenMouseInformationClick(int xPos, int yPos);
    void screenMouseClick(int xPos, int yPos);
    void screenGetTime(char *buffer);
    void drawDirtyScreen(void);
    
    /* External state variables from main.c */
    extern unsigned char isLoaded;
    extern unsigned char playIsPlaying;
}

/* Tile size constant from tiles.h */
#define TILE_SIZE_X 16
#define TILE_SIZE_Y 16

/* Direction constants for screenUpdate() - matches backend.h enum */
#define DIR_LEFT 0
#define DIR_RIGHT 1
#define DIR_UP 2
#define DIR_DOWN 3

/* Drag threshold in pixels - must match main.c */
#define DRAG_THRESHOLD 4

/* Drag state */
static bool s_is_dragging = false;
static bool s_is_mouse_down = false;
static float s_drag_start_x = 0.0f;
static float s_drag_start_y = 0.0f;
static unsigned char s_drag_start_offset_x = 0;
static unsigned char s_drag_start_offset_y = 0;


void imgui_game_viewport_init(void) {
    s_is_dragging = false;
    s_is_mouse_down = false;
    s_drag_start_x = 0.0f;
    s_drag_start_y = 0.0f;
    s_drag_start_offset_x = 0;
    s_drag_start_offset_y = 0;
}

void imgui_game_viewport_render_background(void) {
    /* This function is now a no-op - the game texture is rendered
     * in the main loop via drawBlitGameTexture() which already
     * handles the menu bar offset.
     * 
     * The input processing is handled separately in 
     * imgui_game_viewport_process_input() which is called after
     * ImGui renders to check if input should go to the game.
     */
}

/* Convert screen coordinates to game texture coordinates.
 * Accounts for menu bar offset at the top of the window.
 * Returns 1 if coordinates are within game texture bounds, 0 otherwise.
 */
static int screen_to_game_coords(float screen_x, float screen_y, 
                                  int *game_x, int *game_y) {
    float menu_bar_height = imgui_get_menu_bar_height();
    
    /* Subtract menu bar offset from Y coordinate */
    *game_x = (int)screen_x;
    *game_y = (int)(screen_y - menu_bar_height);
    
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

int imgui_game_viewport_process_input(void) {
    ImGuiIO& io = ImGui::GetIO();
    int input_processed = 0;
    
    /* Only process input if a log is loaded */
    if (isLoaded == 0) {
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
    
    /* Process mouse input */
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        if (!s_is_mouse_down) {
            /* Mouse just went down - start tracking */
            s_is_mouse_down = true;
            s_is_dragging = false;
            s_drag_start_x = io.MousePos.x;
            s_drag_start_y = io.MousePos.y;
            screenGetOffsets(&s_drag_start_offset_x, &s_drag_start_offset_y);
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
            
            /* If dragging, pan the map */
            if (s_is_dragging) {
                int tile_delta_x = (int)dx / TILE_SIZE_X;
                int tile_delta_y = (int)dy / TILE_SIZE_Y;
                
                unsigned char new_x = (unsigned char)(s_drag_start_offset_x - tile_delta_x);
                unsigned char new_y = (unsigned char)(s_drag_start_offset_y - tile_delta_y);
                
                drawDirtyScreen();  /* Invalidate cache when view scrolls */
                screenPanToOffsets(new_x, new_y);
                input_processed = 1;
            }
        }
    }
    
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        if (s_is_mouse_down) {
            if (s_is_dragging) {
                /* End of drag - finalize position */
                float dx = io.MousePos.x - s_drag_start_x;
                float dy = io.MousePos.y - s_drag_start_y;
                
                int tile_delta_x = (int)dx / TILE_SIZE_X;
                int tile_delta_y = (int)dy / TILE_SIZE_Y;
                
                unsigned char new_x = (unsigned char)(s_drag_start_offset_x - tile_delta_x);
                unsigned char new_y = (unsigned char)(s_drag_start_offset_y - tile_delta_y);
                
                drawDirtyScreen();  /* Invalidate cache when view scrolls */
                screenPanToOffsets(new_x, new_y);
            } else {
                /* Click without drag - handle as selection */
                int game_x, game_y;
                if (screen_to_game_coords(io.MousePos.x, io.MousePos.y, &game_x, &game_y)) {
                    /* Check mode to determine which click function to use */
                    if (imgui_get_mode_information()) {
                        screenMouseInformationClick(game_x, game_y);
                    } else {
                        screenMouseClick(game_x, game_y);
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
                drawDirtyScreen();  /* Invalidate cache when view offset changes */
                screenMouseCentreClick(game_x, game_y);
                input_processed = 1;
            }
        }
    }
    
    /* Keyboard input - arrow keys for scrolling */
    if (!io.WantCaptureKeyboard) {
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
            drawDirtyScreen();  /* Invalidate cache when view scrolls */
            screenUpdate(DIR_LEFT);
            input_processed = 1;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
            drawDirtyScreen();
            screenUpdate(DIR_RIGHT);
            input_processed = 1;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
            drawDirtyScreen();
            screenUpdate(DIR_UP);
            input_processed = 1;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
            drawDirtyScreen();
            screenUpdate(DIR_DOWN);
            input_processed = 1;
        }
    }
    
    return input_processed;
}