/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * imgui_ios.cpp — iOS ImGui settings overlay for WinBolo.
 * Minimal touch-friendly overlay: gear icon toggles a
 * settings panel with zoom, sound, and FPS display.
 *
 * Uses SDL3 + SDL_Renderer ImGui backends (same as desktop).
 * Font is scaled up for touch input.
 */

#include <SDL3/SDL.h>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "../imgui_fonts.h"

extern "C" {
#include "global.h"
#include "../lang.h"
}

#include "imgui_ios.h"

/* -------------------------------------------------------
 * External state from winbolo_ios.m
 * ------------------------------------------------------- */
extern "C" BYTE  zoomFactor;
extern "C" bool  soundEffects;
extern "C" int   soundVolume;
extern "C" bool  isInMenu;
extern "C" void  windowSetSoundVolume(int pct);

/* Zoom constants (from winbolo.h) */
#ifndef ZOOM_FACTOR_NORMAL
#define ZOOM_FACTOR_NORMAL 1
#define ZOOM_FACTOR_DOUBLE 2
#define ZOOM_FACTOR_QUAD   4
#endif

extern "C" void windowZoomChange(BYTE amount, bool fromDragResize = false);

/* -------------------------------------------------------
 * Local state
 * ------------------------------------------------------- */
static SDL_Window   *s_window   = nullptr;
static SDL_Renderer *s_renderer = nullptr;
static bool          s_showSettings = false;
static BYTE          s_pendingZoom  = 0;

/* -------------------------------------------------------
 * Public API
 * ------------------------------------------------------- */

bool imguiIosSetup(SDL_Window *window, SDL_Renderer *renderer) {
    if (ImGui::GetCurrentContext()) {
        s_window   = window;
        s_renderer = renderer;
        return true;
    }

    s_window   = window;
    s_renderer = renderer;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;

    /* Scale font for touch — 20px in the 515x325 logical space
       gives comfortable tap targets (~72+ physical px on iPhone).
       Use the shared Inter loader so non-Latin glyph ranges work
       (configured in imguiBoloGlyphRanges()). */
    io.FontGlobalScale = 1.0f;
    imguiLoadBoloFontSized(20.0f);

    ImGui::StyleColorsDark();

    /* Increase frame padding for larger touch targets */
    ImGuiStyle &style = ImGui::GetStyle();
    style.FramePadding  = ImVec2(8.0f, 6.0f);
    style.ItemSpacing   = ImVec2(8.0f, 6.0f);
    style.WindowRounding = 6.0f;
    style.FrameRounding  = 4.0f;
    /* Semi-transparent background so game is visible behind */
    style.Colors[ImGuiCol_WindowBg].w = 0.92f;

    if (!ImGui_ImplSDL3_InitForSDLRenderer(window, renderer)) return false;
    if (!ImGui_ImplSDLRenderer3_Init(renderer))               return false;

    return true;
}

bool imguiIosProcessEvent(SDL_Event *event) {
    if (!s_window) return false;
    ImGui_ImplSDL3_ProcessEvent(event);

    /* If the settings panel is open, consume touch/mouse events */
    if (s_showSettings) {
        ImGuiIO &io = ImGui::GetIO();
        if (io.WantCaptureMouse) {
            switch (event->type) {
            case SDL_EVENT_MOUSE_MOTION:
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
            case SDL_EVENT_MOUSE_WHEEL:
            case SDL_EVENT_FINGER_DOWN:
            case SDL_EVENT_FINGER_UP:
            case SDL_EVENT_FINGER_MOTION:
                return true;
            default:
                break;
            }
        }
    }
    return false;
}

void imguiIosRender(void) {
    if (!s_window || !s_renderer) return;

    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    /* Gear icon button — top-right corner of the 515x325 logical space */
    {
        const float btnSize = 30.0f;
        const float margin  = 6.0f;
        ImGui::SetNextWindowPos(ImVec2(515.0f - btnSize - margin, margin));
        ImGui::SetNextWindowSize(ImVec2(btnSize, btnSize));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0.4f));
        ImGui::Begin("##gear", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_AlwaysAutoResize);

        /* Use a Unicode gear character */
        if (ImGui::Button("*", ImVec2(btnSize - 2, btnSize - 2))) {
            s_showSettings = !s_showSettings;
            isInMenu = s_showSettings;
        }

        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
    }

    /* Settings panel */
    if (s_showSettings) {
        ImGui::SetNextWindowPos(ImVec2(515.0f / 2.0f, 325.0f / 2.0f),
                                ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(220, 0), ImGuiCond_Appearing);

        ImGui::Begin(langGetText(STR_DLGSETTINGS_TITLE), &s_showSettings,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize);

        /* Update isInMenu when the X button is clicked */
        isInMenu = s_showSettings;

        /* --- Zoom --- */
        ImGui::SeparatorText(langGetText(STR_DLGSETTINGS_ZOOM));
        {
            bool z1 = (zoomFactor == ZOOM_FACTOR_NORMAL);
            bool z2 = (zoomFactor == ZOOM_FACTOR_DOUBLE);
            bool z4 = (zoomFactor == ZOOM_FACTOR_QUAD);

            if (ImGui::RadioButton("1x", z1)) s_pendingZoom = ZOOM_FACTOR_NORMAL;
            ImGui::SameLine();
            if (ImGui::RadioButton("2x", z2)) s_pendingZoom = ZOOM_FACTOR_DOUBLE;
            ImGui::SameLine();
            if (ImGui::RadioButton("4x", z4)) s_pendingZoom = ZOOM_FACTOR_QUAD;
        }

        /* --- Sound --- */
        ImGui::SeparatorText(langGetText(STR_DLGSETTINGS_SOUND));
        ImGui::Checkbox(langGetText(STR_MENU_SOUND_EFFECTS), &soundEffects);
        {
            int vol = soundVolume;
            ImGui::SetNextItemWidth(200.0f);
            if (ImGui::SliderInt(langGetText(STR_MENU_VOLUME), &vol, 0, 100, "%d%%")) {
                windowSetSoundVolume(vol);
            }
        }

        /* --- FPS --- */
        ImGui::SeparatorText(langGetText(STR_DLGSETTINGS_PERFORMANCE));
        {
            MessageArgs args = {};
            args.number = (int)(ImGui::GetIO().Framerate + 0.5f);
            ImGui::TextUnformatted(langGetTextFmt(STR_DLGSETTINGS_FPS_FMT, &args));
        }

        ImGui::End();
    }

    ImGui::Render();
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), s_renderer);
}

unsigned char imguiIosConsumePendingZoom(void) {
    BYTE zoom = s_pendingZoom;
    s_pendingZoom = 0;
    return zoom;
}

bool imguiIosWantsInput(void) {
    if (!s_showSettings) return false;
    if (!ImGui::GetCurrentContext()) return false;
    ImGuiIO &io = ImGui::GetIO();
    return io.WantCaptureMouse;
}

void imguiIosCleanup(void) {
    if (!s_window) return;
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    s_window   = nullptr;
    s_renderer = nullptr;
    s_showSettings = false;
}
