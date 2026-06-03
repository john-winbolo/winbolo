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
#include "dialogs/dialog_footer.h"   /* C++ (namespace WBUI) — outside extern "C" */

extern "C" {
#include "global.h"
#include "map_preview_popup.h"
#include "map_preview_view.h"
#include "macos_pinch.h"
#include "../lang.h"
}

/* Singleton popup state. */
static bool             g_popupOpen        = false;
static MapPreviewView  *g_popupView        = NULL;
/* Set when the user clicks "Change" — the lobby polls this each
 * frame via mapPreviewPopupConsumeChangeRequest to know it should
 * open the Choose Map dialog. */
static bool             g_changeRequested  = false;
/* Controls whether the "Change" button renders. Lobby sets this
 * per-frame based on local edit authority (host / admin / openHost). */
static bool             g_showChangeButton = true;

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
    if (!g_popupOpen) return;
    /* Non-modal so the chat / ready / team UI behind it stays
     * interactive. Default geometry mirrors the Choose Map dialog
     * (small top/left gutter, height leaves ~3 chat lines visible
     * at the bottom) so the popup never covers the chat — but the
     * user can drag and resize it and the new geometry sticks
     * across re-opens (ImGui retains per-window state via the
     * "Map Preview" ID). */
    {
        /* Default geometry matches the Choose Map dialog
         * (imgui_lobby.cpp lobbyChooseMapRenderWindow): full screen minus
         * a 15px gutter and ~3 lines at the bottom, with the same min
         * sizes and 0.85-height cap. */
        ImVec2 displaySize = ImGui::GetIO().DisplaySize;
        const float kGutter = 15.0f;
        float lineH = ImGui::GetTextLineHeightWithSpacing();
        float winW = displaySize.x - kGutter * 2.0f;
        float winH = displaySize.y - kGutter * 2.0f - lineH * 3.0f;
        if (winW < 480.0f) winW = 480.0f;
        if (winH < 320.0f) winH = 320.0f;
        if (winH > displaySize.y * 0.85f) winH = displaySize.y * 0.85f;
        ImGui::SetNextWindowSize(ImVec2(winW, winH),  ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos (ImVec2(kGutter, kGutter),
                                 ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSizeConstraints(ImVec2(320.0f, 240.0f),
                                            ImVec2(FLT_MAX, FLT_MAX));
    }
    bool windowOpen = ImGui::Begin("Map Preview##full", &g_popupOpen,
                                   ImGuiWindowFlags_NoScrollbar
                                   | ImGuiWindowFlags_NoScrollWithMouse);
    if (windowOpen) {
        /* Esc only closes when this window has focus — without that
         * guard, hitting Escape anywhere else in the lobby would
         * close the preview unexpectedly. */
        if (ImGui::IsWindowFocused() &&
            ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            g_popupOpen = false;
        }
        /* Reserve room below the image for the DialogFooter — the button
         * row PLUS the separator + spacing it draws above the row. (Just
         * one frame-height clipped the buttons in this NoScrollbar
         * window.) The image fills everything above. */
        float btnRowH = ImGui::GetFrameHeightWithSpacing()
                      + ImGui::GetStyle().ItemSpacing.y * 3.0f + 2.0f;
        ImVec2 full = ImGui::GetContentRegionAvail();
        ImVec2 contentSize(full.x, full.y - btnRowH);
        if (contentSize.y < 64.0f) contentSize.y = 64.0f;

        /* Render the offscreen at the actual content rect size now
         * that we know it (excludes title bar / window padding /
         * button-row reservation). Done inside the modal — mid-frame
         * SDL_SetRenderTarget is safe here, same pattern the chooser
         * uses. */
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
            /* Zoom indicator overlay — aligned to the bottom-right of
             * the IMAGE rect, sitting just above the button row so it
             * doesn't overlap. */
            char zoomText[16];
            SDL_snprintf(zoomText, sizeof(zoomText), "%.2fx",
                         mapPreviewViewGetZoom(g_popupView));
            ImVec2 textSize = ImGui::CalcTextSize(zoomText);
            ImVec2 windowPos = ImGui::GetWindowPos();
            ImVec2 windowSize = ImGui::GetWindowSize();
            float pad = 8.0f;
            ImVec2 textPos(
                windowPos.x + windowSize.x - textSize.x - pad
                    - ImGui::GetStyle().WindowPadding.x,
                windowPos.y + windowSize.y - textSize.y - pad
                    - ImGui::GetStyle().WindowPadding.y - btnRowH);
            ImDrawList *dl = ImGui::GetWindowDrawList();
            ImVec2 bgMin(textPos.x - 4, textPos.y - 2);
            ImVec2 bgMax(textPos.x + textSize.x + 4, textPos.y + textSize.y + 2);
            dl->AddRectFilled(bgMin, bgMax, IM_COL32(0, 0, 0, 160), 4.0f);
            dl->AddText(textPos, IM_COL32(255, 255, 255, 220), zoomText);
        } else {
            macOSPinchZoomConsume(); /* drain so it doesn't jump when data arrives */
            ImGui::Text("Map preview loading...");
        }

        /* Footer row — standard 2-button dialog footer (dialog_footer.h:
         * back-out/Close on the left, primary on the right). When the
         * lobby grants map-change permission we show [Close] [Choose map];
         * otherwise it's a lone view-only [Close]. "Choose map" latches a
         * request the lobby polls (mapPreviewPopupConsumeChangeRequest) to
         * open its Choose Map dialog. */
        if (g_showChangeButton) {
            int f = WBUI::DialogFooter(langGetText(STR_CLOSE),
                                       langGetText(STR_DLGLOBBY_CHOOSE_MAP_BTN));
            if (f == WBUI::FOOTER_CONFIRM) {
                g_changeRequested = true;
                g_popupOpen       = false;
            } else if (f == WBUI::FOOTER_CANCEL) {
                g_popupOpen = false;
            }
        } else {
            if (WBUI::DialogFooter(nullptr, langGetText(STR_CLOSE)) != WBUI::FOOTER_NONE) {
                g_popupOpen = false;
            }
        }
    }
    ImGui::End();
}

void mapPreviewPopupClose(void) {
    if (g_popupOpen) {
        g_popupOpen = false;
    }
}

bool mapPreviewPopupIsOpen(void) {
    return g_popupOpen;
}

void mapPreviewPopupRefreshOpen(const BYTE *compressedData, int compressedLen) {
    if (!g_popupOpen) return;
    if (!compressedData || compressedLen <= 0) return;
    ensureView();
    if (!g_popupView) return;
    /* Keep the user's current zoom/pan across the in-place reload —
     * the user opened this popup intentionally and is mid-interaction,
     * so snapping back to auto-fit would feel jarring. */
    mapPreviewViewLoadCompressedKeepCamera(g_popupView,
                                            compressedData,
                                            compressedLen);
}

void mapPreviewPopupSetShowChange(bool show) {
    g_showChangeButton = show;
}

bool mapPreviewPopupConsumeChangeRequest(void) {
    bool v = g_changeRequested;
    g_changeRequested = false;
    return v;
}

void mapPreviewPopupDestroy(void) {
    if (g_popupView) {
        mapPreviewViewDestroy(g_popupView);
        g_popupView = NULL;
    }
    g_popupOpen = false;
}
