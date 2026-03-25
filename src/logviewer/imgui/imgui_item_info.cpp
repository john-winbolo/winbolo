/*
 * imgui_item_info.cpp - ImGui item information window for Log Viewer
 *
 * Copyright (c) 2024
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "imgui_item_info.h"
#include "imgui_main_menu.h"
#include "imgui_context.h"
#include "imgui.h"

#include <cstdio>
#include <cstring>

/* External functions from backend - using C types directly */
extern "C" {
    void screenCentreOnSelectedItem(void);
    void screenGetPlayerName(char *buffer, unsigned char player);
}

/* Item info state */
static unsigned char s_item_type = 0;      /* 0 = none, 1 = base, 2 = pillbox */
static unsigned char s_item_number = 0;
static unsigned char s_owner = 0;
static unsigned char s_x = 0;
static unsigned char s_y = 0;
static unsigned char s_armour = 0;
static unsigned char s_shells = 0;
static unsigned char s_mines = 0;
static int s_in_tank = 0;

/* Cached display strings */
static char s_owner_text[80] = "";
static char s_location_text[32] = "";

void imgui_item_info_init(void) {
    s_item_type = 0;
    s_item_number = 0;
    s_owner = 0;
    s_x = 0;
    s_y = 0;
    s_armour = 0;
    s_shells = 0;
    s_mines = 0;
    s_in_tank = 0;
    s_owner_text[0] = '\0';
    s_location_text[0] = '\0';
}

void imgui_item_info_update(unsigned char itemType, unsigned char itemNumber, unsigned char owner, 
                            unsigned char x, unsigned char y, unsigned char armour, unsigned char shells, 
                            unsigned char mines, int inTank) {
    /* Update state */
    s_item_type = itemType;
    s_item_number = itemNumber;
    s_owner = owner;
    s_x = x;
    s_y = y;
    s_armour = armour;
    s_shells = shells;
    s_mines = mines;
    s_in_tank = inTank;
    
    /* Update display strings */
    if (itemType == 0) {
        s_owner_text[0] = '\0';
        s_location_text[0] = '\0';
    } else {
        /* Owner text */
        if (owner == 0xFF) { /* NEUTRAL */
            snprintf(s_owner_text, sizeof(s_owner_text), "Neutral");
        } else {
            char player_name[64];
            screenGetPlayerName(player_name, owner);
            snprintf(s_owner_text, sizeof(s_owner_text), "%s (%d)", player_name, owner);
        }
        
        /* Location text */
        snprintf(s_location_text, sizeof(s_location_text), "X: %d, Y: %d", x, y);
    }
}

void imgui_item_info_window(void) {
    if (!g_show_item_info_window) {
        return;
    }
    
    ImGui::SetNextWindowSize(ImVec2(200, 150), ImGuiCond_FirstUseEver);
    
    if (ImGui::Begin("Item Information", &g_show_item_info_window, ImGuiWindowFlags_NoCollapse)) {
        /* Reposition window relative to right/bottom edge when viewport is resized */
        float dx, dy;
        if (imgui_context_get_resize_delta(&dx, &dy)) {
            ImVec2 pos = ImGui::GetWindowPos();
            ImVec2 size = ImGui::GetWindowSize();
            ImVec2 vp = ImGui::GetMainViewport()->Size;
            ImVec2 new_pos = pos;
            if (pos.x + size.x * 0.5f > (vp.x - dx) * 0.5f) new_pos.x = pos.x + dx;
            if (pos.y + size.y * 0.5f > (vp.y - dy) * 0.5f) new_pos.y = pos.y + dy;
            if (new_pos.x < 0) new_pos.x = 0;
            if (new_pos.y < 0) new_pos.y = 0;
            if (new_pos.x + size.x > vp.x) new_pos.x = vp.x - size.x;
            if (new_pos.y + size.y > vp.y) new_pos.y = vp.y - size.y;
            if (new_pos.x != pos.x || new_pos.y != pos.y) ImGui::SetWindowPos(new_pos);
        }

        if (s_item_type == 0) {
            /* No item selected */
            ImGui::Text("No item selected");
        } else {
            /* Item type header */
            if (s_item_type == 1) {
                ImGui::Text("Base #%d", s_item_number);
            } else {
                ImGui::Text("Pillbox #%d", s_item_number);
            }
            
            ImGui::Separator();
            
            /* Location */
            ImGui::Text("Location: %s", s_location_text);
            
            /* Owner */
            ImGui::Text("Owner: %s", s_owner_text);
            
            /* Armour */
            ImGui::Text("Armour: %d", s_armour);
            
            /* Shells (bases only) */
            if (s_item_type == 1) {
                ImGui::Text("Shells: %d", s_shells);
                ImGui::Text("Mines: %d", s_mines);
            }
            
            /* In tank status */
            ImGui::Text("In Tank: %s", s_in_tank ? "Yes" : "No");
            
            /* Center on map button */
            ImGui::Spacing();
            if (s_in_tank) {
                ImGui::BeginDisabled();
            }
            if (ImGui::Button("Center on Map", ImVec2(-1, 0))) {
                screenCentreOnSelectedItem();
            }
            if (s_in_tank) {
                ImGui::EndDisabled();
            }
        }
    }
    ImGui::End();
}