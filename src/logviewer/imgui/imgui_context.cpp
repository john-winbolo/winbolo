/*
 * imgui_context.cpp - ImGui context setup and management for Log Viewer
 *
 * Copyright (c) 2024
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "imgui_context.h"
#include "imgui.h"
#include "../../gui/imgui_theme.h"
#include "../../gui/imgui_fonts.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include <stdio.h>

static ImGuiContext* g_context = nullptr;
static SDL_Window* g_window = nullptr;
static SDL_Renderer* g_renderer = nullptr;

static ImVec2 s_prev_display_size = {0, 0};
static ImVec2 s_resize_delta = {0, 0};

int lv_imgui_context_init(SDL_Window* window, SDL_Renderer* renderer) {
    g_window = window;
    g_renderer = renderer;
    
    // Create ImGui context
    IMGUI_CHECKVERSION();
    g_context = ImGui::CreateContext();
    if (!g_context) {
        return 0;
    }
    ImGui::SetCurrentContext(g_context);

    /* Route ImGui::TextLinkOpenURL() through SDL_OpenURL so clicked links
     * launch the system browser. Without this, TextLinkOpenURL renders
     * but the click does nothing. */
    ImGui::GetPlatformIO().Platform_OpenInShellFn =
        [](ImGuiContext *, const char *url) -> bool {
            return url && *url && SDL_OpenURL(url);
        };
    
    // Enable docking and multi-viewport for flexible window layout
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    // Note: ViewportsEnable requires proper renderer support, may need to be disabled initially
    
    // Set up style
    ImGui::StyleColorsDark();
    imguiApplyBoloTheme();
    
    // Initialize SDL3 backend
    if (!ImGui_ImplSDL3_InitForSDLRenderer(window, renderer)) {
        ImGui::DestroyContext(g_context);
        g_context = nullptr;
        return 0;
    }
    // Initialize SDL Renderer backend
    if (!ImGui_ImplSDLRenderer3_Init(renderer)) {
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext(g_context);
        g_context = nullptr;
        return 0;
    }
    // Load Bolo UI font (builds the font atlas internally)
    imguiLoadBoloFont(18.0f);
    
    return 1;
}

void lv_imgui_context_shutdown(void) {
    if (g_context) {
        ImGui::SetCurrentContext(g_context);
        ImGui_ImplSDLRenderer3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext(g_context);
        g_context = nullptr;
    }
    g_window = nullptr;
    g_renderer = nullptr;
}

void lv_imgui_context_newframe(void) {
    if (!g_context) return;

    ImGui::SetCurrentContext(g_context);
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    /* Track display size changes for resize-relative window positioning */
    ImVec2 current_size = ImGui::GetIO().DisplaySize;
    if (s_prev_display_size.x > 0 && s_prev_display_size.y > 0) {
        s_resize_delta.x = current_size.x - s_prev_display_size.x;
        s_resize_delta.y = current_size.y - s_prev_display_size.y;
    } else {
        s_resize_delta.x = 0;
        s_resize_delta.y = 0;
    }
    s_prev_display_size = current_size;
}

void lv_imgui_context_render(void) {
    if (!g_context) return;
    
    ImGui::SetCurrentContext(g_context);
    ImGui::Render();
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), g_renderer);
}

int lv_imgui_context_handle_event(SDL_Event* event) {
    if (!g_context) return 0;
    
    ImGui::SetCurrentContext(g_context);
    return ImGui_ImplSDL3_ProcessEvent(event) ? 1 : 0;
}

ImGuiContext* lv_imgui_context_get(void) {
    return g_context;
}

int lv_imgui_context_get_resize_delta(float* dx, float* dy) {
    if (dx) *dx = s_resize_delta.x;
    if (dy) *dy = s_resize_delta.y;
    return (s_resize_delta.x != 0.0f || s_resize_delta.y != 0.0f) ? 1 : 0;
}

int lv_imgui_want_capture_mouse(void) {
    if (!g_context) return 0;
    ImGui::SetCurrentContext(g_context);
    return ImGui::GetIO().WantCaptureMouse ? 1 : 0;
}