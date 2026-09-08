/*
 * Copyright (c) 1998-2026 John Morrison.
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
#include <cfloat>   /* FLT_MAX — unbounded text measure */
#include <cmath>    /* sqrtf */

#include <SDL3/SDL.h>

#include "imgui.h"
#include "dialogs/dialog_footer.h"        /* C++ (namespace WBUI) — outside extern "C" */
#include "dialogs/imgui_dialog_utils.h"   /* imguiPush/PopNearestSampling */

extern "C" {
#include "global.h"
#include "map_preview_popup.h"
#include "map_preview_view.h"
#include "macos_pinch.h"
#include "../lang.h"
#include "client_sim.h"   /* clientSimGetLobbySlot, ClientLobbySlot */
#include "client_net.h"   /* clientSimNetSendLobbyClaimStart */
#include "client_command.h"  /* START_CLAIM_TEAM_SIDE — the holder menu's Team side action */
#include "start_sides.h"     /* startSideMaskFor / startSideBits — side masks over the parsed starts */
#include "minimap_render.h"  /* MINIMAP_OWNER_OFFSIDE — dims an off-side start's boat and dot */
#include "../ui_mode.h"   /* uiShouldUseControllerMode */
}
#include "lobby_start_markers.h"   /* shared start-ownership marker helpers and side rules */
#include "lobby_start_list.h"      /* controller-mode non-spatial start list */

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

/* Drag-to-move state. >=0 while dragging a claim to a new start:
 * g_startDragSlot is the lobby slot whose claim is moving, g_startDragFrom
 * the 1-based start it began on. */
static int              g_startDragSlot = -1;
static int              g_startDragFrom = -1;
/* 1-based start the right-click "assign to someone" menu is acting on. */
static int              g_assignMenuStart = -1;

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

/* Zoom level a "jump to this square" lands on. 2x is the same step
 * mapPreviewViewSetInitialBounds opens at, so a jump looks like a normal
 * open that happens to be somewhere specific. Only ever raises the zoom —
 * a user already zoomed in further keeps their level. */
#define POPUP_FOCUS_MIN_ZOOM 2.0f

void mapPreviewPopupFocusMapSquare(const BYTE *compressedData, int compressedLen,
                                   int boundsMinX, int boundsMinY,
                                   int boundsMaxX, int boundsMaxY,
                                   int mapSqX, int mapSqY) {
    /* Already open: keep the loaded map (and the user's zoom) and just
     * travel — reloading would flash the whole view for no reason. */
    if (!g_popupOpen) {
        mapPreviewPopupOpenCompressed(compressedData, compressedLen,
                                      boundsMinX, boundsMinY,
                                      boundsMaxX, boundsMaxY);
    }
    if (!g_popupOpen || !g_popupView) return;
    mapPreviewViewCenterOnMapSquare(g_popupView, mapSqX, mapSqY,
                                    POPUP_FOCUS_MIN_ZOOM);
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

/* 1-based start nearest the cursor within a comfortable pixel radius, among
 * starts currently visible in the popup image; -1 if none. Shared by the
 * pan-suppression pre-pass and the overlay's interaction so both agree on
 * what the pointer is over. */
static int startPickerHoverStart(ImVec2 imgMin, ImVec2 contentSize) {
    if (!g_popupView) return -1;
    int texW = 0, texH = 0;
    mapPreviewViewGetTextureSize(g_popupView, &texW, &texH);
    if (texW <= 0 || texH <= 0) return -1;
    BYTE numStarts = mapPreviewViewGetStartCount(g_popupView);
    int cap = numStarts > MAX_STARTS ? MAX_STARTS : numStarts;
    ImVec2 mp = ImGui::GetMousePos();
    int best = -1;
    float bestD2 = 0.0f;
    for (int i = 1; i <= cap; i++) {
        BYTE mx, my;
        if (!mapPreviewViewGetStart(g_popupView, (BYTE)i, &mx, &my)) continue;
        float tx, ty;
        if (!mapPreviewViewWorldToScreen(g_popupView, mx, my, &tx, &ty)) continue;
        float sx = imgMin.x + (tx / (float)texW) * contentSize.x;
        float sy = imgMin.y + (ty / (float)texH) * contentSize.y;
        if (sx < imgMin.x || sx > imgMin.x + contentSize.x ||
            sy < imgMin.y || sy > imgMin.y + contentSize.y) continue;
        /* Hit area = the (invisible) ~5-tile-diameter circle, min 80 px,
         * scaling with zoom. Radius = half of that. */
        float hitR = 40.0f;  /* 80 px diameter minimum */
        float bx, by;
        if (mapPreviewViewWorldToScreen(g_popupView, mx + 1, my, &bx, &by)) {
            float spxPerTile = fabsf((bx - tx) / (float)texW * contentSize.x);
            float r = 2.5f * spxPerTile;
            if (r > hitR) hitR = r;
        }
        float ex = sx - mp.x, ey = sy - mp.y, d2 = ex * ex + ey * ey;
        if (d2 > hitR * hitR) continue;
        if (best < 0 || d2 < bestD2) { best = i; bestD2 = d2; }
    }
    return best;
}

/* Draw a highlight ring at a single 1-based start, mapped into the displayed
 * image rect — used in controller mode to show which start the focusable list
 * has focus on. Same world->screen path as startPickerHoverStart. No-op when
 * the start isn't mapped (e.g. minimap zoom) or falls off the image rect. */
static void highlightStartOnMap(ImVec2 imgMin, ImVec2 contentSize, int start1) {
    if (!g_popupView || start1 < 1) return;
    int texW = 0, texH = 0;
    mapPreviewViewGetTextureSize(g_popupView, &texW, &texH);
    if (texW <= 0 || texH <= 0) return;
    BYTE mx, my;
    if (!mapPreviewViewGetStart(g_popupView, (BYTE)start1, &mx, &my)) return;
    float tx, ty;
    if (!mapPreviewViewWorldToScreen(g_popupView, mx, my, &tx, &ty)) return;
    float sx = imgMin.x + (tx / (float)texW) * contentSize.x;
    float sy = imgMin.y + (ty / (float)texH) * contentSize.y;
    if (sx < imgMin.x || sx > imgMin.x + contentSize.x ||
        sy < imgMin.y || sy > imgMin.y + contentSize.y) return;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddCircle(ImVec2(sx, sy), 16.0f, IM_COL32(255, 255, 255, 235), 0, 3.0f);
    dl->AddCircle(ImVec2(sx, sy), 16.0f, IM_COL32(0, 0, 0, 160), 0, 1.0f);
}

/* True when the start picker wants the left-drag (a drag is active, or the
 * cursor is over a movable claimed start), so the caller can stop
 * mapPreviewViewHandleInput from panning the map out from under the drag. */
static bool startPickerWantsDrag(ImVec2 imgMin, ImVec2 contentSize,
                                 bool imgHovered) {
    if (g_startDragSlot >= 0) return true;
    if (!imgHovered || !g_startPickerCs) return false;
    int hov = startPickerHoverStart(imgMin, contentSize);
    if (hov < 1) return false;
    int holder = startHolderSlot(g_startPickerCs, (BYTE)hov);
    return holder >= 0 &&
           (g_startPickerEffectiveHost || holder == g_startPickerMySlot);
}

/* Side mask of every start the popup has parsed — the same startSideMaskFor
 * the server and the lobby's cache apply, over the bounding box of the
 * parsed starts. masks[] is 1-based (MAX_STARTS + 1 entries); returns the
 * start count, 0 when no map has parsed. */
static int startPickerSideMasks(BYTE *masks) {
    if (!g_popupView) return 0;
    int n = mapPreviewViewGetStartCount(g_popupView);
    if (n > MAX_STARTS) n = MAX_STARTS;
    BYTE xs[MAX_STARTS + 1], ys[MAX_STARTS + 1];
    bool have[MAX_STARTS + 1];
    int minX = 255, minY = 255, maxX = 0, maxY = 0;
    for (int i = 1; i <= n; i++) {
        have[i] = mapPreviewViewGetStart(g_popupView, (BYTE)i, &xs[i], &ys[i]);
        if (!have[i]) continue;
        if (xs[i] < minX) minX = xs[i];
        if (xs[i] > maxX) maxX = xs[i];
        if (ys[i] < minY) minY = ys[i];
        if (ys[i] > maxY) maxY = ys[i];
    }
    for (int i = 1; i <= n; i++) {
        masks[i] = have[i] ? startSideMaskFor(xs[i], ys[i], minX, minY, maxX, maxY)
                           : 0;
    }
    return n;
}

/* Fill owners[] (0-based, start index i+1) from the picker context so the map
 * render can colour each start: 0=unclaimed,1=self,2=ally,3=enemy, with
 * MINIMAP_OWNER_OFFSIDE set on a start the viewer's own team side rejects so
 * the render dims it. The view redraws its offscreen from this array every
 * frame, so a side change shows at once. Returns the count (0 when there's
 * no lobby context, e.g. a non-lobby caller). */
static int startPickerComputeOwners(uint8_t *owners, int maxN) {
    ClientSim *cs = g_startPickerCs;
    if (!cs || !g_popupView) return 0;
    int n = mapPreviewViewGetStartCount(g_popupView);
    if (n > maxN) n = maxN;
    BYTE masks[MAX_STARTS + 1] = {0};
    int  maskCount = startPickerSideMasks(masks);
    int  myTeam    = lobbySlotTeam(cs, g_startPickerMySlot);
    for (int i = 1; i <= n; i++) {
        int holder = startHolderSlot(cs, (BYTE)i);
        uint8_t o = (uint8_t)lobbyStartClassify(cs, holder, g_startPickerMySlot);
        if (i <= maskCount && lobbyStartOffSide(cs, myTeam, masks[i])) {
            o |= MINIMAP_OWNER_OFFSIDE;
        }
        owners[i - 1] = o;
    }
    return n;
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
    int capStarts = numStarts > MAX_STARTS ? MAX_STARTS : numStarts;

    /* Ownership colouring lives in the map render (boat sprite / dot colour),
     * not here — this overlay draws holder name labels and the hover ring,
     * and drives the pointer interaction. */
    int hover = startPickerHoverStart(imgMin, contentSize);

    /* Side mask of every start, and whether the viewer's own team side
     * rejects it: such a start draws dimmed, and a player who is not the
     * host can neither click nor drop onto it. */
    BYTE masks[MAX_STARTS + 1] = {0};
    int  maskCount = startPickerSideMasks(masks);
    int  myTeam    = lobbySlotTeam(cs, g_startPickerMySlot);
    /* Off-side for a given team; the overlay judges by the viewer's team,
     * a drag by the dragged player's. */
    auto offSideFor = [&](int st, int teamId) {
        return st >= 1 && st <= maskCount && lobbyStartOffSide(cs, teamId, masks[st]);
    };
    auto offSideAt = [&](int st) { return offSideFor(st, myTeam); };

    bool  shown[MAX_STARTS + 1] = {false};
    float scrX[MAX_STARTS + 1], scrY[MAX_STARTS + 1];
    int   holderOf[MAX_STARTS + 1];

    for (int i = 1; i <= capStarts; i++) {
        holderOf[i] = -1;
        BYTE mx, my;
        if (!mapPreviewViewGetStart(g_popupView, (BYTE)i, &mx, &my)) continue;
        holderOf[i] = startHolderSlot(cs, (BYTE)i);
        float texX, texY;
        /* Invalid at minimap zoom — labels simply hide (don't drift). */
        if (!mapPreviewViewWorldToScreen(g_popupView, mx, my, &texX, &texY)) continue;
        float sx = imgMin.x + (texX / (float)texW) * contentSize.x;
        float sy = imgMin.y + (texY / (float)texH) * contentSize.y;
        /* Off the image rect — don't bleed labels past the panel. */
        if (sx < imgMin.x || sx > imgMin.x + contentSize.x ||
            sy < imgMin.y || sy > imgMin.y + contentSize.y) continue;
        shown[i] = true; scrX[i] = sx; scrY[i] = sy;

        /* Holder name (full, as in main) above the boat, or "(open)" for a
         * free start. The boat itself is already coloured by ownership. The
         * ONLY hover style change is "(open)" going full white. */
        const char *label;
        ImU32 fg, bg;
        if (holderOf[i] >= 0) {
            label = clientSimGetLobbySlot(cs, (BYTE)holderOf[i])->playerName;
            /* Name coloured by ownership: ally green, enemy red, else white. */
            LobbyStartOwner o = lobbyStartClassify(cs, holderOf[i], g_startPickerMySlot);
            fg = (o == LSO_ALLY)  ? IM_COL32(120, 230, 120, 255)
               : (o == LSO_ENEMY) ? IM_COL32(235, 90, 90, 255)
                                  : IM_COL32(255, 255, 255, 255);
            bg = IM_COL32(0, 0, 0, 185);
        } else {
            label = langGetText(STR_DLGLOBBY_START_OPEN);
            bool hot = (i == hover);
            /* Hover brightens the "(open)" label, but stays short of full
             * white so it doesn't read as harshly. */
            fg = hot ? IM_COL32(235, 235, 235, 215) : IM_COL32(220, 220, 220, 150);
            bg = hot ? IM_COL32(0, 0, 0, 130)       : IM_COL32(0, 0, 0, 90);
        }
        /* Off-side for the viewer's team: half the label's alpha, matching
         * the faded boat under it. */
        if (offSideAt(i)) {
            fg = lobbyStartDimColor(fg);
            bg = lobbyStartDimColor(bg);
        }
        ImVec2 ts = ImGui::CalcTextSize(label);
        ImVec2 p(sx - ts.x * 0.5f, sy - ts.y - 6.0f);  /* centred above boat */
        dl->AddRectFilled(ImVec2(p.x - 3, p.y - 1),
                          ImVec2(p.x + ts.x + 3, p.y + ts.y + 1), bg, 3.0f);
        dl->AddText(p, fg, label);
    }

    /* ---- Pointer interaction ----
     * Left-click a start = choose it for yourself. Drag a movable claimed
     * start = move that player to another start. Right-click (with edit
     * permission, or on a start you hold) = the assign / holder menu. A
     * start that the placed player's team side rejects — the dragged
     * player during a drag, the viewer otherwise — takes neither a click
     * nor a drop from a player who is not the host — the server would
     * refuse the claim — and its tooltip says so; the host keeps every
     * action and is told which team the start is off-side for. */
    ImVec2 mp     = ImGui::GetMousePos();
    bool   host   = g_startPickerEffectiveHost;
    int    mySlot = g_startPickerMySlot;
    /* Hover tooltip ~75% transparent so it doesn't block the map. */
    auto translucentTooltip = [&](const char *text) {
        ImVec4 pbg = ImGui::GetStyleColorVec4(ImGuiCol_PopupBg); pbg.w *= 0.25f;
        ImVec4 pbd = ImGui::GetStyleColorVec4(ImGuiCol_Border);  pbd.w *= 0.25f;
        ImGui::PushStyleColor(ImGuiCol_PopupBg, pbg);
        ImGui::PushStyleColor(ImGuiCol_Border, pbd);
        ImGui::SetTooltip("%s", text);
        ImGui::PopStyleColor(2);
    };

    if (g_startDragSlot >= 0) {
        /* Esc cancels the drag without reassigning. */
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            g_startDragSlot = -1;
            g_startDragFrom = -1;
            return;
        }
        ImVec2 dd = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
        bool moved = (dd.x * dd.x + dd.y * dd.y >= 16.0f);
        /* The drop target is judged against the dragged player's team, not
         * the viewer's: the host may be moving someone on another team.
         * Only the host may drop anyone on an off-side start. */
        bool dropOffSide = offSideFor(hover, lobbySlotTeam(cs, g_startDragSlot));
        bool dropBlocked = dropOffSide && !host;
        /* While actually dragging, carry the player's NAME on the cursor
         * (the start boats are static). */
        if (moved) {
            const ClientLobbySlot *ds = clientSimGetLobbySlot(cs, (BYTE)g_startDragSlot);
            if (ds && ds->playerName[0]) {
                const char *nm = ds->playerName;
                ImVec2 ts = ImGui::CalcTextSize(nm);
                ImVec2 p(mp.x + 12.0f, mp.y - ts.y * 0.5f);
                dl->AddRectFilled(ImVec2(p.x - 3, p.y - 1),
                                  ImVec2(p.x + ts.x + 3, p.y + ts.y + 1),
                                  IM_COL32(0, 0, 0, 200), 3.0f);
                dl->AddText(p, IM_COL32(255, 255, 255, 255), nm);
            }
            ImGui::SetMouseCursor(dropBlocked ? ImGuiMouseCursor_Arrow
                                              : ImGuiMouseCursor_ResizeAll);
            if (dropOffSide) {
                char tip[192];
                lobbyStartOffSideTip(cs, g_startDragSlot, host, hover, tip, sizeof(tip));
                translucentTooltip(tip);
            }
        }
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            /* A real drag reassigns the dragged player to the target. A
             * no-move click on an occupied start does nothing (choosing a
             * start for yourself is only via clicking a FREE start). */
            if (moved && hover >= 1 && !dropBlocked)
                clientSimNetSendLobbyClaimStart(cs, (BYTE)g_startDragSlot,
                                                (BYTE)hover);
            g_startDragSlot = -1;
            g_startDragFrom = -1;
        }
        return;
    }

    if (imgHovered && hover >= 1) {
        int  holder      = startHolderSlot(cs, (BYTE)hover);
        bool free        = (holder < 0);
        bool mine        = (holder == mySlot);
        bool offSide     = offSideAt(hover);
        /* Nothing to do on an off-side start unless you are the host or
         * already hold it — your own claim can still be dragged away. */
        bool blocked     = offSide && !host && !mine;
        bool canGrab     = (holder >= 0) && (host || mine);  /* drag to move */
        bool canClick    = free && mySlot >= 0 && !blocked;  /* click to choose */

        ImGui::SetMouseCursor(canGrab  ? ImGuiMouseCursor_ResizeAll
                            : canClick ? ImGuiMouseCursor_Hand
                                       : ImGuiMouseCursor_Arrow);

        /* Tooltip reflects what's actually possible: no "click to choose"
         * for a start someone else holds (left-click does nothing there),
         * and an off-side start says so in place of the usual text. */
        char tip[192];
        MessageArgs targs = {};
        targs.number = hover;
        if (offSide) {
            lobbyStartOffSideTip(cs, mySlot, host, hover, tip, sizeof(tip));
        } else if (free) {
            SDL_snprintf(tip, sizeof(tip), "%s",
                         langGetTextFmt(host ? STR_STARTPICK_TIP_FREE_HOST
                                             : STR_STARTPICK_TIP_FREE, &targs));
        } else {
            const char *who = mine ? langGetText(STR_STARTPICK_YOU)
                : clientSimGetLobbySlot(cs, (BYTE)holder)->playerName;
            SDL_strlcpy(targs.playerName, who, sizeof(targs.playerName));
            SDL_snprintf(tip, sizeof(tip), "%s",
                         langGetTextFmt(host ? STR_STARTPICK_TIP_HELD_HOST
                                             : STR_STARTPICK_TIP_HELD, &targs));
        }
        translucentTooltip(tip);

        /* Right-click opens the menu: the host on any start, a holder on
         * the start they hold. */
        if ((host || (holder >= 0 && mine)) &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            g_assignMenuStart = hover;
            ImGui::OpenPopup("##assignStart");
        }

        if (canGrab) {
            /* Press begins a potential drag-to-move; a no-move click does
             * nothing (handled in the drag branch above). */
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                g_startDragSlot = holder;
                g_startDragFrom = hover;
            }
        } else if (canClick && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            /* Free start: a plain click chooses it for you. */
            ImVec2 dd = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
            if (dd.x * dd.x + dd.y * dd.y < 16.0f)
                clientSimNetSendLobbyClaimStart(cs, (BYTE)mySlot, (BYTE)hover);
        }
    }

    /* Menu on a start. A held start leads with two entries for its holder:
     * Team side hands the start back and has the server re-pick on the
     * holder's side at once (Auto for a team with no side); Unassign frees
     * it and leaves the holder unplaced until the next lobby event. Then,
     * for the host, a separator and the assign list — every connected tank,
     * grouped by team with a divider between teams, a player whose side
     * rejects this start suffixed "(off-side)" but still assignable. A
     * holder who is not the host sees only the two entries; on a free start
     * only the host's list appears. */
    if (ImGui::BeginPopup("##assignStart")) {
        int  st       = g_assignMenuStart;
        int  holder   = (st >= 1) ? startHolderSlot(cs, (BYTE)st) : -1;
        bool occupied = (holder >= 0);
        if (occupied) {
            BYTE holderSide = lobbyTeamSide(cs, lobbySlotTeam(cs, holder));
            char actLbl[64];
            if (startSideBits(holderSide) != 0) {
                SDL_snprintf(actLbl, sizeof(actLbl), "%s \xC2\xB7 %s##teamSide",
                             langGetText(STR_DLGLOBBY_START_TEAM_SIDE),
                             langGetText(lobbySideCompassId(holderSide)));
            } else {
                SDL_snprintf(actLbl, sizeof(actLbl), "%s##teamSide",
                             langGetText(STR_DLGLOBBY_START_AUTO));
            }
            if (ImGui::Selectable(actLbl)) {
                clientSimNetSendLobbyClaimStart(cs, (BYTE)holder, START_CLAIM_TEAM_SIDE);
                ImGui::CloseCurrentPopup();
            }
            SDL_snprintf(actLbl, sizeof(actLbl), "%s##unassign",
                         langGetText(STR_DLGLOBBY_START_UNASSIGN));
            if (ImGui::Selectable(actLbl)) {
                clientSimNetSendLobbyClaimStart(cs, (BYTE)holder, 0xFF);
                ImGui::CloseCurrentPopup();
            }
        }
        if (host) {
            if (occupied) ImGui::Separator();
            ImGui::TextDisabled("%s", langGetText(occupied ? STR_STARTPICK_SWAP_WITH
                                                           : STR_STARTPICK_ASSIGN_TO));
            ImGui::Separator();
            bool firstGroup = true;
            for (int team = 0; team <= 15; team++) {
                bool groupOpened = false;
                for (int k = 0; k < MAX_TANKS; k++) {
                    const ClientLobbySlot *sl = clientSimGetLobbySlot(cs, (BYTE)k);
                    if (!sl || !sl->connected) continue;
                    if ((int)sl->teamNumber != team) continue;
                    if (!groupOpened) {
                        if (!firstGroup) ImGui::Separator();  /* HR between teams */
                        firstGroup  = false;
                        groupOpened = true;
                    }
                    /* The host may still hand out an off-side start; the
                     * suffix says this player's side rejects it. */
                    bool offSideFor = st >= 1 && st <= maskCount &&
                                      lobbyStartOffSide(cs, team, masks[st]);
                    char lbl[128];
                    SDL_snprintf(lbl, sizeof(lbl), "%s%s%s##assign%d",
                                 sl->playerName[0] ? sl->playerName
                                     : langGetText(STR_STARTPICK_SLOT_FALLBACK),
                                 offSideFor ? " " : "",
                                 offSideFor ? langGetText(STR_DLGLOBBY_START_OFFSIDE_SUFFIX)
                                            : "",
                                 k);
                    if (ImGui::Selectable(lbl) && st >= 1) {
                        clientSimNetSendLobbyClaimStart(cs, (BYTE)k, (BYTE)st);
                        ImGui::CloseCurrentPopup();
                    }
                }
            }
        }
        ImGui::EndPopup();
    }
}

void mapPreviewPopupRenderModal(SDL_Renderer *renderer) {
    (void)renderer;
    if (!g_popupOpen) {
        g_startPickerCs     = NULL;  /* consume even when closed */
        g_startPickerMySlot = -1;
        g_startDragSlot     = -1;    /* never resume a drag across re-open */
        g_startDragFrom     = -1;
        g_assignMenuStart   = -1;
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

        /* Controller mode swaps the spatial zoom/pan/click picker for a
         * focusable start list down the left, with the map kept on the
         * right as a highlight-only view of the focused start. The pad
         * drives the list (Space/A claims); no wheel/drag/zoom. */
        bool controllerPicker = uiShouldUseControllerMode() && g_startPickerCs;
        float listW = 0.0f;
        ImVec2 mapSize = contentSize;
        if (controllerPicker) {
            listW = contentSize.x * 0.34f;
            if (listW < 200.0f) listW = 200.0f;
            if (listW > contentSize.x - 120.0f) listW = contentSize.x - 120.0f;
            if (listW < 0.0f) listW = 0.0f;
            mapSize.x = contentSize.x - listW - ImGui::GetStyle().ItemSpacing.x;
            if (mapSize.x < 64.0f) mapSize.x = 64.0f;
        }

        /* Render the offscreen at the actual map rect size now
         * that we know it (excludes title bar / window padding /
         * button-row reservation, and the list column in controller
         * mode). Done inside the modal — mid-frame SDL_SetRenderTarget
         * is safe here, same pattern the chooser uses. */
        if (mapSize.x > 0 && mapSize.y > 0 && g_popupView) {
            /* Colour each start by ownership before the boats/dots render. */
            uint8_t owners[MAX_STARTS];
            int nOwn = startPickerComputeOwners(owners, MAX_STARTS);
            mapPreviewViewSetStartOwners(g_popupView, nOwn ? owners : NULL, nOwn);
            mapPreviewViewRenderOffscreen(g_popupView, renderer,
                                           (int)mapSize.x,
                                           (int)mapSize.y);
        }
        if (mapPreviewViewIsReady(g_popupView)) {
            SDL_Texture *tex = mapPreviewViewGetTexture(g_popupView);
            if (tex && controllerPicker) {
                /* Focusable start list on the left; the pad navigates it and
                 * activates a free start to claim. The map on the right is
                 * highlight-only — no wheel/drag/zoom input is fed. */
                int startCount = mapPreviewViewGetStartCount(g_popupView);
                /* The list applies the same side rules as the marker
                 * overlay, so a start the pad cannot take reads off-side
                 * in both places. */
                BYTE listMasks[MAX_STARTS + 1] = {0};
                startPickerSideMasks(listMasks);
                /* NavFlattened so the list's rows live in the popup window's
                 * focus scope — the pad navigates straight into them and
                 * B/Escape still closes the popup in one press (no extra
                 * nav-cancel step backing out of a child first). */
                ImGui::BeginChild("##StartListCol", ImVec2(listW, contentSize.y),
                                  ImGuiChildFlags_NavFlattened);
                int focusedStart = lobbyStartListRender(g_startPickerCs,
                                                        g_startPickerMySlot,
                                                        startCount, listMasks);
                ImGui::EndChild();
                ImGui::SameLine();
                ImVec2 imgMin = ImGui::GetCursorScreenPos();
                bool nearest = mapPreviewViewWantsNearestSampling(g_popupView);
                if (nearest) imguiPushNearestSampling();
                ImGui::Image((ImTextureID)tex, mapSize);
                if (nearest) imguiPopNearestSampling();
                highlightStartOnMap(imgMin, mapSize, focusedStart);
            } else if (tex) {
                ImVec2 imgMin = ImGui::GetCursorScreenPos();
                /* Point-sample the zoomed map: the widget's offscreen is
                 * already built at the display size, so bilinear here only
                 * re-blended crisp tile art across the sub-pixel gap between
                 * the integer texture and the float Image rect. */
                bool nearest = mapPreviewViewWantsNearestSampling(g_popupView);
                if (nearest) imguiPushNearestSampling();
                ImGui::Image((ImTextureID)tex, contentSize);
                if (nearest) imguiPopNearestSampling();
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
                /* Yield the left-drag to the start picker when over a movable
                 * marker (or mid-drag) so the map doesn't pan under the drag. */
                bool panSuppress = startPickerWantsDrag(imgMin, contentSize, imgHovered);
                MapPreviewInputOpts opts = { !panSuppress, true, true, true };
                mapPreviewViewHandleInput(g_popupView, imgHovered, &opts);
                renderStartPickerOverlay(imgMin, contentSize, imgHovered);

                /* Controller, spatial view (no start-picker list): the pad
                 * pans via the D-pad (arrowPan, fed as arrow keys) but has no
                 * wheel to zoom, so overlay two focusable zoom buttons. "+"/"-"
                 * are punctuation, not localizable captions. Hidden in mouse
                 * mode (wheel zoom) and in the controller list view above. */
                if (uiShouldUseControllerMode() && !g_startPickerCs) {
                    /* Placing the buttons moves the cursor to the image
                     * top-left; save it first and restore it last so the
                     * footer below still lays out at the bottom (where the
                     * pan InvisibleButton left the cursor), matching mouse
                     * mode exactly. */
                    ImVec2 afterSpatial = ImGui::GetCursorScreenPos();
                    float btn = ImGui::GetFrameHeight();
                    float pad = 8.0f;
                    ImGui::SetCursorScreenPos(ImVec2(imgMin.x + pad, imgMin.y + pad));
                    ImGui::SetNextItemAllowOverlap();
                    if (ImGui::Button("+##zoomIn", ImVec2(btn, btn)))
                        mapPreviewViewZoomIn(g_popupView);
                    ImGui::SetCursorScreenPos(
                        ImVec2(imgMin.x + pad, imgMin.y + pad + btn + 4.0f));
                    ImGui::SetNextItemAllowOverlap();
                    if (ImGui::Button("-##zoomOut", ImVec2(btn, btn)))
                        mapPreviewViewZoomOut(g_popupView);
                    ImGui::SetCursorScreenPos(afterSpatial);
                }
            }
            /* Zoom indicator overlay — aligned to the bottom-right of
             * the IMAGE rect, sitting just above the button row so it
             * doesn't overlap. Hidden in controller mode (no zoom). */
            if (!controllerPicker) {
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
            }
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
    g_assignMenuStart = -1;
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
