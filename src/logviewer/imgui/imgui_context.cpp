/*
 * imgui_context.cpp - ImGui context setup and management for Log Viewer
 *
 * Copyright (c) 1998-2026 John Morrison.
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#include "imgui_context.h"
#include "spectator_drain.h"   /* SPEC_PHASE_* discriminants for the badge */
#include "imgui.h"
#include "../../gui/imgui_theme.h"
#include "../../gui/imgui_fonts.h"
#include "../../gui/ui_mode.h"
#include "../../gui/lang.h"
#include "../../gui/sdl3/dialogs/imgui_nav_outline.h"
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
    /* No imgui.ini, like every other ImGui context in the tree. Left on, ImGui
     * writes it into the process's current directory, which is wherever the
     * viewer was launched from (a log double-clicked on the Desktop, say).
     * Window visibility is already kept in the viewer's own preferences. */
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.ConfigNavCursorVisibleAlways = true;
    // Note: ViewportsEnable requires proper renderer support, may need to be disabled initially

    // Set up style
    ImGui::StyleColorsDark();
    imguiApplyBoloTheme();

    /* Tablet/Deck: bigger hit targets and scrollbars for touch + Deck
       readability. Mirrors sdl3imgui.cpp:2410-2419. The lv_drawGetZoomFactor
       analogue is not exposed to this TU, so use ps=1.0f for now. */
    if (uiModeIsTablet() || uiModeIsSteamDeck()) {
        float ps = 1.0f;  /* TODO: scale by zoom factor */
        ImGuiStyle &style = ImGui::GetStyle();
        style.FramePadding      = ImVec2(12 * ps, 8 * ps);
        style.ItemSpacing       = ImVec2(12 * ps, 8 * ps);
        style.TouchExtraPadding = ImVec2(8 * ps, 8 * ps);
        style.ScrollbarSize     = 24.0f * ps;
        if (uiModeIsTablet()) {
            io.ConfigFlags |= ImGuiConfigFlags_IsTouchScreen;
            io.FontGlobalScale = 1.8f * ps;
        }
    }
    
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
    dialogDrawNavOutline();
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

void lv_imgui_set_gamepad_nav(int enabled) {
    if (!g_context) return;
    ImGui::SetCurrentContext(g_context);
    ImGuiIO& io = ImGui::GetIO();
    if (enabled) {
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    } else {
        io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
    }
}

int lv_imgui_any_popup_open(void) {
    if (!g_context) return 0;
    ImGui::SetCurrentContext(g_context);
    return ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId |
                                       ImGuiPopupFlags_AnyPopupLevel) ? 1 : 0;
}

void lv_imgui_center_message(const char* text) {
    if (!g_context || text == nullptr) return;
    ImGui::SetCurrentContext(g_context);
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImVec2 centre(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f);
    ImGui::SetNextWindowPos(centre, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::Begin("##spec_overlay", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_AlwaysAutoResize |
                 ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextUnformatted(text);
    ImGui::End();
}

void lv_imgui_spectator_badge(int phase) {
    if (!g_context) return;
    const char* label;
    ImU32 col;
    switch (phase) {
        case SPEC_PHASE_LOBBY:
        case SPEC_PHASE_COUNTDOWN: label = langGetText(STR_LV_SPEC_PHASE_LOBBY);    col = IM_COL32( 70, 130, 200, 255); break;
        case SPEC_PHASE_RUNNING:   label = langGetText(STR_LV_SPEC_PHASE_LIVE);     col = IM_COL32( 60, 175,  75, 255); break;
        case SPEC_PHASE_GAMEOVER:  label = langGetText(STR_LV_SPEC_PHASE_GAMEOVER); col = IM_COL32(190,  75,  75, 255); break;
        default: return;   /* SPEC_PHASE_UNKNOWN: no badge */
    }
    ImGui::SetCurrentContext(g_context);
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    /* Top-right corner, nudged below any menu bar so the game-view menu doesn't
     * cover it. Pivot on the badge's top-right edge. */
    ImVec2 pos(vp->Pos.x + vp->Size.x - 8.0f, vp->Pos.y + 30.0f);
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGui::Begin("##spec_badge", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_AlwaysAutoResize |
                 ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    ImGui::End();
}