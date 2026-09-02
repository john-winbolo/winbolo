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
 * Name:          lobby_map_preview.cpp
 * Purpose:       The lobby's map preview. The compressed map
 *                bytes stashed when the map lands, and the
 *                256x256 minimap texture built from them.
 *                The per-start cache — compass octant label,
 *                map square and bounding box — rebuilt from
 *                those bytes on each map change so the
 *                overlay and the player list's start column
 *                never decompress the map per frame. The
 *                caption and bar shown while the map is not
 *                yet in hand. The start-ownership markers
 *                drawn over the preview image, with the
 *                holder's minimal unique name prefix placed
 *                toward the map edge. And the click and drag
 *                handling over the image that claims a free
 *                start or moves a claimed one.
 *********************************************************/

#include <cstdlib>  /* abs — compass octant deltas */
#include <cstring>  /* memset / memcpy — start caches, holder name prefix */
#include <cmath>    /* atan2 / floor — start compass octant math */
#include <cfloat>   /* FLT_MAX — unbounded wrap width for CalcTextSizeA */

#include <SDL3/SDL.h>

#include "imgui.h"
#include "lobby_internal.h"
#include "../../lobby_start_markers.h"  /* lobbyStartHolderSlot / Classify, compass helpers */
extern "C" {
#include "client_sim.h"  /* ClientSim + lobby getters; ClientLobbySlot */
#include "client_net.h"  /* clientSimGetConnectState, clientSimNetSendLobbyClaimStart */
#include "../../minimap_render.h"  /* minimapFromCompressedOwned / MinimapBounds */
#include "../../../../bolo/public/client_mappreview.h"  /* MapPreview + start accessors */
#include "../../../lang.h"  /* langGetText / STR_COMPASS_* / STR_DLGLOBBY_* */
}

static LobbyMapPreviewState s_mapPreview = {};

/* Core reads the stashed map bytes; players reads those and the per-start
 * caches behind the start column and the ownership overlay. */
LobbyMapPreviewState *lobbyMapPreview(void) {
    return &s_mapPreview;
}

/* The compressed map buffer is owned here, so it has to be released before
 * the struct is overwritten. */
void lobbyMapPreviewReset(void) {
    if (s_mapPreview.popupCompressedData) {
        SDL_free(s_mapPreview.popupCompressedData);
    }
    s_mapPreview = LobbyMapPreviewState{};
}

/* Compass octant of a start at (sx,sy) within the start bounding box
 * [minX..maxX, minY..maxY]. Map Y increases downward, so north = smaller
 * y. Returns a STR_* lang id for N/NE/E/SE/S/SW/W/NW, or C (centre) when
 * the start sits within ~1/8 of the bbox extent of the centre on both
 * axes. */
static int lobbyStartCompassStr(int sx, int sy, int minX, int minY,
                                int maxX, int maxY) {
    int cx = (minX + maxX) / 2;
    int cy = (minY + maxY) / 2;
    int dx = sx - cx;
    int dy = sy - cy;
    int tolX = (maxX - minX) / 8; if (tolX < 1) tolX = 1;
    int tolY = (maxY - minY) / 8; if (tolY < 1) tolY = 1;
    if (abs(dx) <= tolX && abs(dy) <= tolY) {
        return STR_COMPASS_C;
    }
    /* atan2 with -dy flips screen-down y back to math-up north. Result in
     * (-180,180]: 0=E, 90=N, 180=W, -90=S. Snap into 8 sectors of 45deg
     * each, biasing by half a sector so each label is centred on its
     * cardinal/intercardinal direction. */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
    double deg = atan2((double)(-dy), (double)dx) * 180.0 / M_PI;
    int sector = (int)floor((deg + 22.5) / 45.0);
    sector = ((sector % 8) + 8) % 8;
    static const int kSectorStr[8] = {
        STR_COMPASS_E,  STR_COMPASS_NE, STR_COMPASS_N,  STR_COMPASS_NW,
        STR_COMPASS_W,  STR_COMPASS_SW, STR_COMPASS_S,  STR_COMPASS_SE,
    };
    return kSectorStr[sector];
}

/* Rebuild s_mapPreview.startCompassId from a runtime compressed map buffer.
 * Loads a transient MapPreview (the same bytes lobbyBuildMapPreview consumes),
 * computes the bounding box over all starts, then fills one compass id per start.
 * Clears the cache on failure or an empty start list. */
void lobbyRebuildStartCompassCache(const BYTE *data, int len) {
    memset(s_mapPreview.startCompassId, 0, sizeof(s_mapPreview.startCompassId));
    memset(s_mapPreview.startMapX, 0, sizeof(s_mapPreview.startMapX));
    memset(s_mapPreview.startMapY, 0, sizeof(s_mapPreview.startMapY));
    s_mapPreview.startCount = 0;
    s_mapPreview.startBboxMinX = s_mapPreview.startBboxMinY = 0;
    s_mapPreview.startBboxMaxX = s_mapPreview.startBboxMaxY = 0;
    MapPreview *mp = clientMapPreviewLoadFromBuffer(data, len);
    if (!mp) {
        return;
    }
    BYTE n = clientMapPreviewGetStartCount(mp);
    if (n == 0) {
        clientMapPreviewDestroy(mp);
        return;
    }
    if (n > MAX_STARTS) n = MAX_STARTS;
    int minX = 255, minY = 255, maxX = 0, maxY = 0;
    BYTE i;
    for (i = 1; i <= n; i++) {
        BYTE x, y, dir;
        if (!clientMapPreviewGetStart(mp, i, &x, &y, &dir)) continue;
        if (x < minX) minX = x;
        if (x > maxX) maxX = x;
        if (y < minY) minY = y;
        if (y > maxY) maxY = y;
    }
    for (i = 1; i <= n; i++) {
        BYTE x, y, dir;
        if (!clientMapPreviewGetStart(mp, i, &x, &y, &dir)) continue;
        s_mapPreview.startMapX[i] = x;
        s_mapPreview.startMapY[i] = y;
        s_mapPreview.startCompassId[i] =
            lobbyStartCompassStr(x, y, minX, minY, maxX, maxY);
    }
    s_mapPreview.startCount    = n;
    s_mapPreview.startBboxMinX = minX;
    s_mapPreview.startBboxMinY = minY;
    s_mapPreview.startBboxMaxX = maxX;
    s_mapPreview.startBboxMaxY = maxY;
    clientMapPreviewDestroy(mp);
}

/* Caption + bar fraction for the map-transfer line the preview panel shows
 * while the map is not yet in hand. Both places that draw it call this so
 * they cannot drift apart.
 *
 * The percentage on its own cannot say "not started". It is computed from
 * the transport's byte counters, and those outlive a transfer: after a
 * mid-lobby map change the client re-JOINs, and until the new JOIN_ACCEPT
 * re-arms them they still describe the PREVIOUS map — fully received, so
 * 100%. Reporting that verbatim is what made a wedged re-join look like a
 * download stuck at the finish line. The connect state is the only thing
 * that separates the two, so read them together.
 *
 * A live-lobby spectator fetching its own copy of the map stays
 * SPECTATING throughout, so it lands on the waiting caption for the
 * duration rather than showing a bar. Counting it as downloading would be
 * worse: a spectator the server sent no map for has no byte total either,
 * and the percentage answers "nothing to fetch" as 100 — a full bar for a
 * transfer that never started, which is the exact thing this is undoing.
 * Telling the two apart needs the transport's spectator-download flag,
 * which no T1 accessor exposes today. */
const char *lobbyMapTransferLine(ClientSim *cs, float *outProgress) {
    if (clientSimGetConnectState(cs) == CLIENT_CONNECT_DOWNLOADING_MAP) {
        *outProgress = (float)clientSimGetMapDownloadPercent(cs) / 100.0f;
        return langGetText(STR_DLGLOBBY_DOWNLOADING);
    }
    *outProgress = 0.0f;
    return langGetText(STR_DLGLOBBY_AWAITING_MAP);
}

/* Per-start ownership codes (0-based, start index i+1) for the minimap
 * colouring: 0=unclaimed, 1=self, 2=ally, 3=enemy. Also returns an FNV-1a
 * signature so the caller can detect when a recolour rebuild is needed
 * (claims/team changes don't trigger a map re-download). Returns the count. */
int lobbyComputeStartOwners(ClientSim *cs, int myPlayerNum,
                                   uint8_t *owners, int maxN, uint32_t *outSig) {
    int n = (int)clientSimGetLobbyStartCount(cs);
    if (n > maxN) n = maxN;
    if (n < 0)    n = 0;
    uint32_t sig = 2166136261u;
    for (int i = 1; i <= n; i++) {
        uint8_t o = (uint8_t)lobbyStartClassify(cs, lobbyStartHolderSlot(cs, i),
                                                myPlayerNum);
        owners[i - 1] = o;
        sig = (sig ^ o) * 16777619u;
    }
    sig = (sig ^ (uint32_t)n) * 16777619u;
    if (outSig) *outSig = sig;
    return n;
}

/* 1-based start whose displayed position is within radiusPx of pt (the
 * ~25px hover catch area), or -1. Shared by the overlay's hover border and
 * the mini-map drag-drop target. */
static int lobbyPreviewStartAtScreen(ImVec2 imgMin, float previewSize,
                                     int bx0, int by0, int bx1, int by1,
                                     ImVec2 pt, float radiusPx) {
    if (s_mapPreview.startCount == 0) return -1;
    float spanX = (float)((bx1 + 1) - bx0);
    float spanY = (float)((by1 + 1) - by0);
    if (spanX <= 0.0f || spanY <= 0.0f) return -1;
    int best = -1;
    float bestD2 = 0.0f;
    float r2 = radiusPx * radiusPx;
    for (int i = 1; i <= (int)s_mapPreview.startCount; i++) {
        float fx = imgMin.x + (((float)s_mapPreview.startMapX[i] + 0.5f - bx0) / spanX) * previewSize;
        float fy = imgMin.y + (((float)s_mapPreview.startMapY[i] + 0.5f - by0) / spanY) * previewSize;
        float ex = fx - pt.x, ey = fy - pt.y, d2 = ex * ex + ey * ey;
        if (d2 > r2) continue;
        if (best < 0 || d2 < bestD2) { best = i; bestD2 = d2; }
    }
    return best;
}

/* Draw reserved-start ownership markers over the inline map preview Image.
 * Maps each cached start map-square into the displayed (cropped) image rect,
 * colours it by who claimed it (self/ally/enemy/unclaimed), and labels
 * claimed starts with the minimal unique prefix of the holder's name placed
 * toward the map edge (per the start's compass octant) so labels avoid the
 * playable centre. (imgMin, previewSize) is the on-screen Image rect; the
 * b* ints are the source-pixel crop the Image's UVs map from. */
void lobbyDrawPreviewStartOverlay(ClientSim *cs, int myPlayerNum,
                                         ImVec2 imgMin, float previewSize,
                                         int bx0, int by0, int bx1, int by1) {
    const bool spectator = clientSimIsSpectator(cs);
    if (s_mapPreview.startCount == 0) return;
    float spanX = (float)((bx1 + 1) - bx0);
    float spanY = (float)((by1 + 1) - by0);
    if (spanX <= 0.0f || spanY <= 0.0f) return;

    /* Pass 1: holder slot + a compact name list for disambiguation. */
    const char *holderNames[MAX_STARTS + 1] = {0};
    int holderOf[MAX_STARTS + 1];
    int nameListIdx[MAX_STARTS + 1];
    int nHolders = 0;
    for (int i = 1; i <= (int)s_mapPreview.startCount; i++) {
        holderOf[i]    = lobbyStartHolderSlot(cs, i);
        nameListIdx[i] = -1;
        if (holderOf[i] >= 0) {
            const char *nm = clientSimGetLobbySlot(cs, (BYTE)holderOf[i])->playerName;
            if (nm && nm[0]) {
                nameListIdx[i] = nHolders;
                holderNames[nHolders++] = nm;
            }
        }
    }

    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImFont *font   = ImGui::GetFont();
    float fsz      = ImGui::GetFontSize() * 0.85f;
    float tilePx   = (spanX > 0.0f) ? (previewSize / spanX) : 1.0f;

    for (int i = 1; i <= (int)s_mapPreview.startCount; i++) {
        /* Start map-square centre -> displayed image pixel. */
        float fx = imgMin.x + (((float)s_mapPreview.startMapX[i] + 0.5f - bx0) / spanX) * previewSize;
        float fy = imgMin.y + (((float)s_mapPreview.startMapY[i] + 0.5f - by0) / spanY) * previewSize;
        if (fx < imgMin.x || fx > imgMin.x + previewSize ||
            fy < imgMin.y || fy > imgMin.y + previewSize) continue;

        /* Label: "#<n>" alone, or "#<n> <initials>" when claimed (the
         * disambiguating prefix to the right of the number). */
        char buf[40];
        if (nameListIdx[i] >= 0) {
            int plen = lobbyStartUniquePrefixLen(holderNames, nHolders, nameListIdx[i]);
            char pfx[24];
            if (plen > (int)sizeof(pfx) - 1) plen = (int)sizeof(pfx) - 1;
            memcpy(pfx, holderNames[nameListIdx[i]], (size_t)plen);
            pfx[plen] = '\0';
            SDL_snprintf(buf, sizeof(buf), "#%d %s", i, pfx);
        } else {
            SDL_snprintf(buf, sizeof(buf), "#%d", i);
        }

        /* Centre the whole field one tile toward the map edge per active
         * compass axis (e.g. a SW start -> one tile down & one tile west),
         * so the centre point is the same whether or not an initial is
         * present. */
        LobbyCompassDir dir = lobbyStartCompassDir(
            s_mapPreview.startMapX[i], s_mapPreview.startMapY[i],
            s_mapPreview.startBboxMinX, s_mapPreview.startBboxMinY,
            s_mapPreview.startBboxMaxX, s_mapPreview.startBboxMaxY);
        float ox, oy;
        lobbyCompassOffset(dir, &ox, &oy);
        int dirX = (ox > 0.3f) ? 1 : (ox < -0.3f ? -1 : 0);
        int dirY = (oy > 0.3f) ? 1 : (oy < -0.3f ? -1 : 0);
        float offPx = tilePx;            /* one tile, but at least 10 px */
        if (offPx < 10.0f) offPx = 10.0f;
        float cxp = fx + (float)dirX * offPx;
        float cyp = fy + (float)dirY * offPx;
        ImVec2 ts = font->CalcTextSizeA(fsz, FLT_MAX, 0.0f, buf);
        /* For a purely E/W label, anchor the near text edge at the offset
         * point so the field grows away from the start (centring would let
         * it grow back over the start); otherwise centre on the point. */
        ImVec2 tp;
        if (dirY == 0 && dirX > 0)        /* East: left edge anchored right */
            tp = ImVec2(fx + offPx, fy - ts.y * 0.5f);
        else if (dirY == 0 && dirX < 0)   /* West: right edge anchored left */
            tp = ImVec2(fx - offPx - ts.x, fy - ts.y * 0.5f);
        else                              /* N/S/diagonal/centre: centred */
            tp = ImVec2(cxp - ts.x * 0.5f, cyp - ts.y * 0.5f);
        /* Colour the whole label by ownership: you/allies a bright mint green
         * (distinct from the grass/forest greens so it stands out), enemies
         * red, unclaimed white. */
        ImU32 txtCol = IM_COL32(255, 255, 255, 255);
        if (nameListIdx[i] >= 0) {
            LobbyStartOwner o = lobbyStartClassify(cs, holderOf[i],
                                                   spectator ? -1 : myPlayerNum);
            if (o == LSO_ENEMY) txtCol = IM_COL32(235, 90, 90, 255);
            else                txtCol = IM_COL32(80, 255, 170, 255);
        }
        dl->AddText(font, fsz, ImVec2(tp.x + 1.0f, tp.y + 1.0f),
                    IM_COL32(0, 0, 0, 205), buf);
        dl->AddText(font, fsz, tp, txtCol, buf);
    }

    /* 1px white border around a start, marking it as "the one in focus":
     * the start under the cursor (~25px catch, also the drag-drop target)
     * and the start whose dropdown entry is currently hovered. */
    auto outlineStart = [&](int st) {
        if (st < 1 || st > (int)s_mapPreview.startCount) return;
        float fx = imgMin.x + (((float)s_mapPreview.startMapX[st] + 0.5f - bx0) / spanX) * previewSize;
        float fy = imgMin.y + (((float)s_mapPreview.startMapY[st] + 0.5f - by0) / spanY) * previewSize;
        float dotPx = 3.0f * previewSize / spanX;
        float half  = dotPx * 0.5f + 1.5f;
        if (half < 4.0f) half = 4.0f;
        dl->AddRect(ImVec2(fx - half, fy - half), ImVec2(fx + half, fy + half),
                    IM_COL32(255, 255, 255, 255), 0.0f, 0, 1.0f);
    };
    outlineStart(lobbyPreviewStartAtScreen(imgMin, previewSize, bx0, by0, bx1, by1,
                                           ImGui::GetMousePos(), 25.0f));
    outlineStart(s_mapPreview.hoveredStartChoice);
    s_mapPreview.hoveredStartChoice = -1;   /* consume */
}

/* Interaction layer for the inline map preview. Called right after the map
 * Image, which stays the last item (so the caller's mapPreviewPopupOnClick
 * still works). No invisible button — that grabbed nav focus and drew a
 * light-blue focus outline. Clicking a FREE start moves you there; pressing a
 * movable claimed start and dragging reassigns its player (a manual drag).
 * Returns true if it consumed the click so the caller skips the zoom popup. */
bool lobbyPreviewInteract(ClientSim *cs, int myPlayerNum, bool effHostMap,
                                 ImVec2 imgMin, float innerSize,
                                 int bx0, int by0, int bx1, int by1) {
    static int s_miniDragHolder = -1;   /* lobby slot being dragged, or -1 */
    bool consumed = false;
    /* A spectator owns no slot: it can neither claim a free start nor drag a
     * claimed one. Bail before any click is interpreted as an action. */
    if (clientSimIsSpectator(cs)) return consumed;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 mp = ImGui::GetMousePos();
    bool hov = ImGui::IsItemHovered();   /* the map Image (last item) */

    if (s_miniDragHolder >= 0) {
        consumed = true;
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            s_miniDragHolder = -1;
            return consumed;
        }
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        const ClientLobbySlot *ds = clientSimGetLobbySlot(cs, (BYTE)s_miniDragHolder);
        if (ds && ds->playerName[0]) {
            ImVec2 ts = ImGui::CalcTextSize(ds->playerName);
            ImVec2 p(mp.x + 12.0f, mp.y - ts.y * 0.5f);
            dl->AddRectFilled(ImVec2(p.x - 3, p.y - 1),
                              ImVec2(p.x + ts.x + 3, p.y + ts.y + 1),
                              IM_COL32(0, 0, 0, 200), 3.0f);
            dl->AddText(p, IM_COL32(255, 255, 255, 255), ds->playerName);
        }
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            int st = lobbyPreviewStartAtScreen(imgMin, innerSize, bx0, by0, bx1, by1,
                                               mp, 25.0f);
            if (st >= 1)
                clientSimNetSendLobbyClaimStart(cs, (BYTE)s_miniDragHolder, (BYTE)st);
            s_miniDragHolder = -1;
        }
        return consumed;
    }

    if (!hov) return consumed;
    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    /* On press: a free start under the cursor → claim it for yourself; a
     * movable claimed start → begin a manual drag-to-move. Both consume the
     * click so the zoom popup doesn't open. */
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        int st = lobbyPreviewStartAtScreen(imgMin, innerSize, bx0, by0, bx1, by1,
                                           mp, 25.0f);
        int holder = (st >= 1) ? lobbyStartHolderSlot(cs, st) : -1;
        if (st >= 1 && holder < 0 && myPlayerNum >= 0) {
            clientSimNetSendLobbyClaimStart(cs, (BYTE)myPlayerNum, (BYTE)st);
            consumed = true;
        } else if (holder >= 0 && (effHostMap || holder == myPlayerNum)) {
            s_miniDragHolder = holder;
            consumed = true;
        }
    }
    return consumed;
}

/* Build a 256x256 RGBA minimap from compressed map data.
 * Returns an SDL_Texture* or NULL on failure.
 * bounds is filled with the bounding box of non-sea terrain. */
SDL_Texture *lobbyBuildMapPreview(SDL_Renderer *renderer,
                                     const BYTE *compressedData, int dataLen,
                                     LobbyMapBounds *bounds,
                                     const uint8_t *startOwners, int ownerCount) {
    MinimapBounds mb;
    SDL_Texture *tex = minimapFromCompressedOwned(renderer, compressedData, dataLen,
                                                  &mb, NULL, NULL, NULL,
                                                  startOwners, ownerCount);
    if (bounds) {
        bounds->minX = mb.minX;
        bounds->minY = mb.minY;
        bounds->maxX = mb.maxX;
        bounds->maxY = mb.maxY;
    }
    return tex;
}
