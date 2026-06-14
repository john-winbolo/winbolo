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
#include "client_sim.h"   /* clientSimGetLobbySlot, ClientLobbySlot */
#include "client_net.h"   /* clientSimNetSendLobbyClaimStart */
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

/* Lobby start-picker context, set per-frame via
 * mapPreviewPopupSetStartPicker and cleared at the end of each
 * RenderModal so a non-lobby frame can't draw labels off a stale cs. */
static ClientSim       *g_startPickerCs           = NULL;
static int              g_startPickerMySlot       = -1;
static bool             g_startPickerEffectiveHost = false;

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

/* Connected slot reserving start i (1-based), or -1 if free. */
static int startHolderSlot(ClientSim *cs, BYTE i) {
    for (int k = 0; k < MAX_TANKS; k++) {
        const ClientLobbySlot *sl = clientSimGetLobbySlot(cs, (BYTE)k);
        if (sl && sl->connected && sl->startIdx == i) return k;
    }
    return -1;
}

/* Draw a reserving-player name (solid) or "(open)" (translucent) beside
 * each start, then claim a free start on a click (not a pan-drag). All
 * positions go through the Phase-2 transform (texture px), mapped into
 * the displayed image rect [imgMin, imgMin + contentSize]. */
static void renderStartPickerOverlay(ImVec2 imgMin, ImVec2 contentSize,
                                     bool imgHovered) {
    ClientSim *cs = g_startPickerCs;
    if (!cs || !g_popupView) return;
    int texW = 0, texH = 0;
    mapPreviewViewGetTextureSize(g_popupView, &texW, &texH);
    if (texW <= 0 || texH <= 0) return;

    BYTE numStarts = mapPreviewViewGetStartCount(g_popupView);
    ImDrawList *dl = ImGui::GetWindowDrawList();

    for (BYTE i = 1; i <= numStarts; i++) {
        BYTE mx, my;
        if (!mapPreviewViewGetStart(g_popupView, i, &mx, &my)) continue;
        float texX, texY;
        /* Invalid at minimap zoom — labels simply hide (don't drift). */
        if (!mapPreviewViewWorldToScreen(g_popupView, mx, my, &texX, &texY))
            continue;
        float sx = imgMin.x + (texX / (float)texW) * contentSize.x;
        float sy = imgMin.y + (texY / (float)texH) * contentSize.y;
        /* Off the image rect — don't bleed labels past the panel. */
        if (sx < imgMin.x || sx > imgMin.x + contentSize.x ||
            sy < imgMin.y || sy > imgMin.y + contentSize.y) continue;

        int holder = startHolderSlot(cs, i);
        const char *label;
        ImU32 fg, bg;
        if (holder >= 0) {
            label = clientSimGetLobbySlot(cs, (BYTE)holder)->playerName;
            fg = IM_COL32(255, 255, 255, 255);
            bg = IM_COL32(0, 0, 0, 185);
        } else {
            label = langGetText(STR_DLGLOBBY_START_OPEN);
            fg = IM_COL32(220, 220, 220, 150);
            bg = IM_COL32(0, 0, 0, 90);
        }
        ImVec2 ts = ImGui::CalcTextSize(label);
        /* Center the pill on the start and nudge it above the boat. */
        ImVec2 p(sx - ts.x * 0.5f, sy - ts.y - 4.0f);
        dl->AddRectFilled(ImVec2(p.x - 3, p.y - 1),
                          ImVec2(p.x + ts.x + 3, p.y + ts.y + 1), bg, 3.0f);
        dl->AddText(p, fg, label);
    }

    /* Click-to-claim: a left release over the image with negligible drag
     * (a real pan-drag exceeds ImGui's drag threshold so GetMouseDragDelta
     * is non-zero and we skip it). Self slot, free starts only. */
    if (!imgHovered || !ImGui::IsMouseReleased(ImGuiMouseButton_Left)) return;
    ImVec2 dd = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
    if (dd.x * dd.x + dd.y * dd.y >= 16.0f) return;  /* a drag, not a click */
    if (g_startPickerMySlot < 0) return;

    ImVec2 m = ImGui::GetMousePos();
    float texX = (m.x - imgMin.x) / contentSize.x * (float)texW;
    float texY = (m.y - imgMin.y) / contentSize.y * (float)texH;
    int mapSqX, mapSqY;
    if (!mapPreviewViewScreenToWorld(g_popupView, texX, texY, &mapSqX, &mapSqY))
        return;

    /* Nearest start to the click, within ~2 map squares. */
    int best = -1;
    long bestD2 = 0;
    for (BYTE i = 1; i <= numStarts; i++) {
        BYTE mx, my;
        if (!mapPreviewViewGetStart(g_popupView, i, &mx, &my)) continue;
        long ex = (long)mx - mapSqX;
        long ey = (long)my - mapSqY;
        long d2 = ex * ex + ey * ey;
        if (best < 0 || d2 < bestD2) { best = i; bestD2 = d2; }
    }
    if (best < 1 || bestD2 > 8) return;          /* nothing close enough */
    if (startHolderSlot(cs, (BYTE)best) >= 0) return;  /* occupied — ignore */
    clientSimNetSendLobbyClaimStart(cs, (BYTE)g_startPickerMySlot, (BYTE)best);
}

void mapPreviewPopupRenderModal(SDL_Renderer *renderer) {
    (void)renderer;
    if (!g_popupOpen) {
        g_startPickerCs     = NULL;  /* consume even when closed */
        g_startPickerMySlot = -1;
        return;
    }
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
                ImVec2 imgMin = ImGui::GetCursorScreenPos();
                ImGui::Image((ImTextureID)tex, contentSize);
                /* Overlay an InvisibleButton on the image rect so a
                 * click-drag pans the map instead of dragging the whole
                 * popup window around the lobby. The button takes the
                 * drag as an active item (which suppresses ImGui's
                 * drag-body-to-move-window); the title bar still moves
                 * the window. Mirrors the Choose Map dialog's preview. */
                ImGui::SetCursorScreenPos(imgMin);
                ImGui::SetNextItemAllowOverlap();
                ImGui::InvisibleButton("##MapPreviewPan", contentSize);
                bool   imgHovered = ImGui::IsItemHovered();
                MapPreviewInputOpts opts = { true, true, true, true };
                mapPreviewViewHandleInput(g_popupView, imgHovered, &opts);
                renderStartPickerOverlay(imgMin, contentSize, imgHovered);
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

    /* Consume the per-frame picker context so the next frame must
     * re-set it (a non-lobby caller of the popup gets no overlay). */
    g_startPickerCs     = NULL;
    g_startPickerMySlot = -1;
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

void mapPreviewPopupSetStartPicker(struct ClientSim *cs, int myPlayerNum,
                                   bool effectiveHost) {
    g_startPickerCs            = cs;
    g_startPickerMySlot        = myPlayerNum;
    g_startPickerEffectiveHost = effectiveHost;
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
