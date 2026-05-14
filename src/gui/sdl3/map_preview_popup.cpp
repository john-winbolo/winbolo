/*
 * Copyright (c) 1998-2008 John Morrison.
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
 * Name:          map_preview_popup.cpp
 * Purpose:       Modal-popup wrapper around MapPreviewView
 *                (see map_preview_view.{h,cpp}). The render
 *                / zoom / pan / pinch / keyboard logic lives
 *                in the widget; this file just owns one
 *                singleton instance and shows it inside an
 *                ImGui::BeginPopupModal so existing call
 *                sites (which use the public API in
 *                map_preview_popup.h) keep working unchanged.
 *********************************************************/

#include <cstdio>
#include <cstring>

#include <SDL3/SDL.h>

#include "imgui.h"

extern "C" {
#include "global.h"
#include "map_preview_popup.h"
#include "map_preview_view.h"
#include "macos_pinch.h"
}

/* Singleton popup state. */
static bool             g_popupOpen        = false;
static MapPreviewView  *g_popupView        = NULL;

static void ensureView(void) {
    if (!g_popupView) g_popupView = mapPreviewViewCreate();
}

void mapPreviewPopupOpenCompressed(const BYTE *compressedData, int compressedLen,
                                   int boundsMinX, int boundsMinY,
                                   int boundsMaxX, int boundsMaxY) {
    if (!compressedData || compressedLen <= 0) return;
    ensureView();
    if (!g_popupView) return;
    mapPreviewViewLoadCompressed(g_popupView, compressedData, compressedLen);
    mapPreviewViewSetInitialBounds(g_popupView,
                                    boundsMinX, boundsMinY,
                                    boundsMaxX, boundsMaxY);
    g_popupOpen = true;
}

void mapPreviewPopupOpenFile(const char *mapPath,
                             int boundsMinX, int boundsMinY,
                             int boundsMaxX, int boundsMaxY) {
    if (!mapPath || mapPath[0] == '\0') return;
    ensureView();
    if (!g_popupView) return;
    mapPreviewViewLoadFile(g_popupView, mapPath);
    mapPreviewViewSetInitialBounds(g_popupView,
                                    boundsMinX, boundsMinY,
                                    boundsMaxX, boundsMaxY);
    g_popupOpen = true;
}

void mapPreviewPopupOnClick(const BYTE *compressedData, int compressedLen,
                            int boundsMinX, int boundsMinY,
                            int boundsMaxX, int boundsMaxY) {
    if (!ImGui::IsItemClicked()) return;
    mapPreviewPopupOpenCompressed(compressedData, compressedLen,
                                  boundsMinX, boundsMinY,
                                  boundsMaxX, boundsMaxY);
}

void mapPreviewPopupOnClickFile(const char *mapPath,
                                int boundsMinX, int boundsMinY,
                                int boundsMaxX, int boundsMaxY) {
    if (!ImGui::IsItemClicked()) return;
    mapPreviewPopupOpenFile(mapPath, boundsMinX, boundsMinY,
                            boundsMaxX, boundsMaxY);
}

void mapPreviewPopupRenderOffscreen(SDL_Renderer *renderer, int winW, int winH) {
    /* Intentionally no-op — the popup's offscreen is rendered INSIDE
     * the modal body (mapPreviewPopupRenderModal) where we know the
     * actual content rect minus title bar / padding. Sizing the
     * offscreen with the raw 0.8x window size pre-NewFrame produced a
     * texture whose aspect ratio didn't match the Image rect once
     * the title bar was accounted for — ImGui then stretched on one
     * axis, drifting the displayed zoom away from a perfect square.
     * Kept as a stub for ABI compatibility with existing callers. */
    (void)renderer; (void)winW; (void)winH;
}

void mapPreviewPopupRenderModal(SDL_Renderer *renderer) {
    (void)renderer;
    if (g_popupOpen && !ImGui::IsPopupOpen("Map Preview##full")) {
        ImGui::OpenPopup("Map Preview##full");
    }
    {
        ImVec2 displaySize = ImGui::GetIO().DisplaySize;
        ImVec2 popupSize(displaySize.x * 0.8f, displaySize.y * 0.8f);
        ImGui::SetNextWindowSize(popupSize, ImGuiCond_Always);
        ImGui::SetNextWindowPos(ImVec2(displaySize.x * 0.5f, displaySize.y * 0.5f),
                                ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    }
    bool modalOpen = ImGui::BeginPopupModal("Map Preview##full", &g_popupOpen,
                               ImGuiWindowFlags_NoScrollbar
                               | ImGuiWindowFlags_NoScrollWithMouse
                               | ImGuiWindowFlags_NoMove);
    if (modalOpen) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            g_popupOpen = false;
            ImGui::CloseCurrentPopup();
        }
        /* Render the offscreen at the actual content rect size now
         * that we know it (excludes title bar / window padding). Done
         * inside the modal — mid-frame SDL_SetRenderTarget is safe
         * here, same pattern the chooser uses. */
        ImVec2 contentSize = ImGui::GetContentRegionAvail();
        if (contentSize.x > 0 && contentSize.y > 0 && g_popupView) {
            mapPreviewViewRenderOffscreen(g_popupView, renderer,
                                           (int)contentSize.x,
                                           (int)contentSize.y);
        }
        if (mapPreviewViewIsReady(g_popupView)) {
            SDL_Texture *tex = mapPreviewViewGetTexture(g_popupView);
            if (tex) {
                ImGui::Image((ImTextureID)tex, contentSize);
                MapPreviewInputOpts opts = { true, true, true, true };
                mapPreviewViewHandleInput(g_popupView,
                                          ImGui::IsItemHovered(), &opts);
            }
            /* Zoom indicator overlay (bottom-right). */
            char zoomText[16];
            SDL_snprintf(zoomText, sizeof(zoomText), "%.2fx",
                         mapPreviewViewGetZoom(g_popupView));
            ImVec2 textSize = ImGui::CalcTextSize(zoomText);
            ImVec2 windowPos = ImGui::GetWindowPos();
            ImVec2 windowSize = ImGui::GetWindowSize();
            float pad = 8.0f;
            ImVec2 textPos(windowPos.x + windowSize.x - textSize.x - pad - ImGui::GetStyle().WindowPadding.x,
                          windowPos.y + windowSize.y - textSize.y - pad - ImGui::GetStyle().WindowPadding.y);
            ImDrawList *dl = ImGui::GetWindowDrawList();
            ImVec2 bgMin(textPos.x - 4, textPos.y - 2);
            ImVec2 bgMax(textPos.x + textSize.x + 4, textPos.y + textSize.y + 2);
            dl->AddRectFilled(bgMin, bgMax, IM_COL32(0, 0, 0, 160), 4.0f);
            dl->AddText(textPos, IM_COL32(255, 255, 255, 220), zoomText);
        } else {
            macOSPinchZoomConsume(); /* drain so it doesn't jump when data arrives */
            ImGui::Text("Map preview loading...");
        }
        ImGui::EndPopup();
    }
}

void mapPreviewPopupClose(void) {
    if (g_popupOpen) {
        g_popupOpen = false;
    }
}

void mapPreviewPopupDestroy(void) {
    if (g_popupView) {
        mapPreviewViewDestroy(g_popupView);
        g_popupView = NULL;
    }
    g_popupOpen = false;
}
