/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
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

#include <cstring>  /* memset / memcpy — start caches, holder name prefix */
#include <cfloat>   /* FLT_MAX — unbounded wrap width for CalcTextSizeA */

#include <SDL3/SDL.h>

#include "imgui.h"
#include "lobby_internal.h"
#include "../../lobby_start_markers.h"  /* lobbyStartHolderSlot / Classify, compass helpers, side rules, off-side tooltip */
#include "../../wb_theme.h"  /* g_theme->teamColors — the compass rose's per-team colours */
#include "start_sides.h"  /* startSideMaskFor — side mask behind the compass label */
#include "lobby_side_axis.h"  /* LOBBY_SIDE_AXIS_* and the compass axis rules */
extern "C" {
#include "client_sim.h"  /* ClientSim + lobby getters; ClientLobbySlot */
#include "client_net.h"  /* clientSimGetConnectState, clientSimNetSendLobbyClaimStart */
#include "../../minimap_render.h"  /* minimapFromCompressedOwned / MinimapBounds / MINIMAP_OWNER_OFFSIDE */
#include "../../../../bolo/public/client_mappreview.h"  /* MapPreview + start accessors */
#include "../../../lang.h"  /* langGetText / STR_COMPASS_* / STR_DLGLOBBY_* */
}

static LobbyMapPreviewState s_mapPreview = {};

/* Lobby slot being dragged across the inline preview, or -1. File scope
 * rather than a local static inside lobbyPreviewInteract because the
 * compass drawn over the same image has to keep its hands off the mouse
 * while a drag is in flight. */
static int s_miniDragHolder = -1;

/* Set with s_miniDragHolder when the host pressed on a start held by
 * someone else (shared starts only): the press may still turn into a drag
 * of that holder, but a release without movement joins the start instead
 * of moving anybody. */
static bool s_miniDragJoin = false;

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

/* Compass label of a start at (sx,sy) within the start bounding box
 * [minX..maxX, minY..maxY], as a STR_* lang id. Read off the side mask
 * startSideMaskFor computes: one bit is a cardinal label (N/E/S/W), two
 * bits a diagonal (NE/SE/SW/NW), no bits the centre band (C). */
static int lobbyStartCompassStr(int sx, int sy, int minX, int minY,
                                int maxX, int maxY) {
    switch (startSideMaskFor(sx, sy, minX, minY, maxX, maxY)) {
        case START_SIDE_BIT_N:                    return STR_COMPASS_N;
        case START_SIDE_BIT_N | START_SIDE_BIT_E: return STR_COMPASS_NE;
        case START_SIDE_BIT_E:                    return STR_COMPASS_E;
        case START_SIDE_BIT_S | START_SIDE_BIT_E: return STR_COMPASS_SE;
        case START_SIDE_BIT_S:                    return STR_COMPASS_S;
        case START_SIDE_BIT_S | START_SIDE_BIT_W: return STR_COMPASS_SW;
        case START_SIDE_BIT_W:                    return STR_COMPASS_W;
        case START_SIDE_BIT_N | START_SIDE_BIT_W: return STR_COMPASS_NW;
        default:                                  return STR_COMPASS_C;
    }
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
    s_mapPreview.startLiveCount = 0;
    s_mapPreview.pillCount = s_mapPreview.pillLiveCount = 0;
    s_mapPreview.baseCount = s_mapPreview.baseLiveCount = 0;
    s_mapPreview.startBboxMinX = s_mapPreview.startBboxMinY = 0;
    s_mapPreview.startBboxMaxX = s_mapPreview.startBboxMaxY = 0;
    MapPreview *mp = clientMapPreviewLoadFromBuffer(data, len);
    if (!mp) {
        return;
    }
    s_mapPreview.pillCount     = clientMapPreviewGetPillCount(mp);
    s_mapPreview.pillLiveCount = clientMapPreviewGetLivePillCount(mp);
    s_mapPreview.baseCount     = clientMapPreviewGetBaseCount(mp);
    s_mapPreview.baseLiveCount = clientMapPreviewGetLiveBaseCount(mp);
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
        s_mapPreview.startLiveCount++;
    }
    s_mapPreview.startCount    = n;
    s_mapPreview.startBboxMinX = minX;
    s_mapPreview.startBboxMinY = minY;
    s_mapPreview.startBboxMaxX = maxX;
    s_mapPreview.startBboxMaxY = maxY;
    clientMapPreviewDestroy(mp);
}

/* Side mask of 1-based start k from the cache above; 0 (centre) when k is
 * off the cached list. The player list's start cell and dropdown and the
 * inline overlay all read the one cache through this. */
BYTE lobbyStartSideMask(int k) {
    if (k < 1 || k > (int)s_mapPreview.startCount) return 0;
    return startSideMaskFor(s_mapPreview.startMapX[k], s_mapPreview.startMapY[k],
                            s_mapPreview.startBboxMinX, s_mapPreview.startBboxMinY,
                            s_mapPreview.startBboxMaxX, s_mapPreview.startBboxMaxY);
}

int lobbyLiveStartCount(ClientSim *cs) {
    int n = (int)clientSimGetLobbyStartCount(cs);
    /* The same slot count is the check that the cache is this map's, not the
     * last one's while the new map's bytes are still arriving. It is only a
     * check on the count: when the last map and the new one have the same
     * number of slots but a different number in the border, the last map's
     * live count shows until the new bytes land and the cache is rebuilt.
     * The same holds for the pill and base counts below. */
    if (s_mapPreview.startCount > 0 && (int)s_mapPreview.startCount == n) {
        return (int)s_mapPreview.startLiveCount;
    }
    return n;
}

int lobbyLivePillCount(ClientSim *cs) {
    int n = (int)clientSimGetLobbyPillCount(cs);
    if (s_mapPreview.pillCount > 0 && (int)s_mapPreview.pillCount == n) {
        return (int)s_mapPreview.pillLiveCount;
    }
    return n;
}

int lobbyLiveBaseCount(ClientSim *cs) {
    int n = (int)clientSimGetLobbyBaseCount(cs);
    if (s_mapPreview.baseCount > 0 && (int)s_mapPreview.baseCount == n) {
        return (int)s_mapPreview.baseLiveCount;
    }
    return n;
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
 * colouring: 0=unclaimed, 1=self, 2=ally, 3=enemy, with
 * MINIMAP_OWNER_OFFSIDE set on a start the viewer's own team side rejects
 * so the dot is drawn dimmed. Also returns an FNV-1a signature so the
 * caller can detect when a recolour rebuild is needed (claims, team and
 * side changes don't trigger a map re-download); the off-side bit is
 * hashed with the code, so a side change moves the signature too.
 * Returns the count. */
int lobbyComputeStartOwners(ClientSim *cs, int myPlayerNum,
                                   uint8_t *owners, int maxN, uint32_t *outSig) {
    int n = (int)clientSimGetLobbyStartCount(cs);
    if (n > maxN) n = maxN;
    if (n < 0)    n = 0;
    int myTeam = lobbySlotTeam(cs, myPlayerNum);
    uint32_t sig = 2166136261u;
    for (int i = 1; i <= n; i++) {
        uint8_t o = (uint8_t)lobbyStartClassifyShared(cs, i, myPlayerNum);
        if (lobbyStartOffSide(cs, myTeam, lobbyStartSideMask(i))) {
            o |= MINIMAP_OWNER_OFFSIDE;
        }
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
        if (s_mapPreview.startCompassId[i] == 0) continue;  /* not on the map */
        float fx = imgMin.x + (((float)s_mapPreview.startMapX[i] + 0.5f - bx0) / spanX) * previewSize;
        float fy = imgMin.y + (((float)s_mapPreview.startMapY[i] + 0.5f - by0) / spanY) * previewSize;
        float ex = fx - pt.x, ey = fy - pt.y, d2 = ex * ex + ey * ey;
        if (d2 > r2) continue;
        if (best < 0 || d2 < bestD2) { best = i; bestD2 = d2; }
    }
    return best;
}

/* A team's colour as the player panel's header strip picks it: the theme
 * colour the team chose once the team is in use, otherwise the palette
 * entry its number cycles onto. */
static ImU32 lobbyCompassTeamColor(ClientSim *cs, int teamId) {
    uint8_t colorIdx = clientSimGetLobbyTeamColor(cs, (BYTE)teamId);
    if (clientSimGetLobbyTeamInUse(cs, (BYTE)teamId) && colorIdx < 8) {
        return g_theme->teamColors[colorIdx];
    }
    return g_theme->teamColors[(teamId - 1) & 7];
}

/* "N/S" or "E/W" from the localized compass letters — the axis name the
 * two compass tooltips lead with. */
static void lobbyCompassAxisLabel(int axis, char *buf, size_t bufLen) {
    BYTE sa = START_SIDE_ANY, sb = START_SIDE_ANY;
    char a[16], b[16];
    lobbySideAxisSides(axis, &sa, &sb);
    SDL_strlcpy(a, langGetText(lobbySideCompassId(sa)), sizeof(a));
    SDL_strlcpy(b, langGetText(lobbySideCompassId(sb)), sizeof(b));
    SDL_snprintf(buf, bufLen, "%s/%s", a, b);
}

/* The two-team compass rose, drawn in the bottom-left corner of the map
 * preview image: N/E/S/W around a centre mark, an axis at a time.
 *
 * It is a shortcut for the pair of "Start:" combos in the player panel's
 * team headers, so it follows them exactly: host only (the combos are not
 * drawn for anyone else, so neither is this), and only when the map has
 * starts. On top of that it needs a pair to act on, so it appears only
 * when exactly two teams have members — the same "connected slots, bots
 * included" count the panel puts in its team headers.
 *
 * Hovering either letter of an axis lights both: N and S together, E and
 * W together. On an axis the teams are not already on, the tooltip offers
 * to put them there and names which team takes which side, and a click
 * sends both sides — the lower team id takes the first side of the axis
 * (north, or east). When the two teams' sides already form that axis it
 * is drawn permanently lit in the two teams' colours, and the tooltip and
 * click instead put both teams back to Any.
 *
 * Returns true when the rose is under the cursor, so the caller skips the
 * start claim/drag layer and the zoom popup for that click. */
/* Everything the rose needs before it can draw or answer the mouse: the
 * two teams, the centre and radius, the four letter anchors and, from the
 * cursor, which axis (if any) is under it. Shared by the hit test that runs
 * BEFORE the start claim/drag layer (so the rose keeps mouse priority) and
 * the draw that runs AFTER the start overlay (so the letters paint on top
 * of any start label pushed into this corner). */
struct LobbyCompassGeom {
    int    teamA, teamB;
    ImVec2 c;
    float  r;
    ImVec2 gp[4];      /* letter anchors, N E S W */
    int    setAxis;    /* axis the two teams are on now, or NONE */
    int    hovAxis;    /* axis under the cursor, or NONE */
    bool   overRose;   /* cursor within the rose's reach at all */
};
static const BYTE kCompassSides[4] = { START_SIDE_N, START_SIDE_E,
                                       START_SIDE_S, START_SIDE_W };

static bool lobbyCompassGeom(ClientSim *cs, bool effHostMap, ImVec2 imgMin,
                             float innerSize, float gapPx, float s,
                             LobbyCompassGeom *g) {
    /* Same gates as the "Start:" combo, plus the pair the rose acts on. */
    if (!effHostMap) return false;
    if (s_mapPreview.startCount == 0) return false;
    if (s_miniDragHolder >= 0) return false;   /* a drag owns the mouse */
    if (!lobbyTwoTeamPair(cs, &g->teamA, &g->teamB)) return false;

    /* Corner rose, sized off the preview but held between a legible
     * minimum and a modest maximum so it stays a badge rather than an
     * overlay. Give up entirely on a preview too small to hold one. */
    float r = innerSize * 0.13f;
    if (r < 20.0f * s) r = 20.0f * s;
    if (r > 40.0f * s) r = 40.0f * s;
    if (r * 2.0f > innerSize * 0.45f) return false;
    /* Top-left corner of the WHOLE preview box -- the deep-blue inset
     * (gapPx wide on every side) included, not just the map image -- as
     * tight as the glyph letters allow. */
    const float pad = 2.0f * s;
    g->r = r;
    g->c = ImVec2(imgMin.x - gapPx + pad + r, imgMin.y - gapPx + pad + r);

    /* Letter positions, in the N, E, S, W order of the side values. */
    const float gr = r * 0.66f;
    g->gp[0] = ImVec2(g->c.x,      g->c.y - gr);
    g->gp[1] = ImVec2(g->c.x + gr, g->c.y);
    g->gp[2] = ImVec2(g->c.x,      g->c.y + gr);
    g->gp[3] = ImVec2(g->c.x - gr, g->c.y);

    g->setAxis  = lobbyTeamPairAxis(cs, g->teamA, g->teamB);
    g->hovAxis  = LOBBY_SIDE_AXIS_NONE;
    g->overRose = false;
    if (ImGui::IsWindowHovered()) {
        ImVec2 mp = ImGui::GetMousePos();
        float dx = mp.x - g->c.x, dy = mp.y - g->c.y;
        float reach = r + 3.0f * s;
        g->overRose = (dx * dx + dy * dy) <= reach * reach;
        float hit = r * 0.42f;
        if (hit < 9.0f * s) hit = 9.0f * s;
        for (int i = 0; i < 4; i++) {
            if (mp.x >= g->gp[i].x - hit && mp.x <= g->gp[i].x + hit &&
                mp.y >= g->gp[i].y - hit && mp.y <= g->gp[i].y + hit) {
                g->hovAxis  = lobbySideAxisOfSide(kCompassSides[i]);
                g->overRose = true;
                break;
            }
        }
    }
    return true;
}

bool lobbyPreviewCompassHot(ClientSim *cs, bool effHostMap,
                            ImVec2 imgMin, float innerSize, float gapPx, float s) {
    LobbyCompassGeom g;
    if (!lobbyCompassGeom(cs, effHostMap, imgMin, innerSize, gapPx, s, &g)) return false;
    return g.overRose;
}

bool lobbyDrawPreviewCompass(ClientSim *cs, bool effHostMap,
                             ImVec2 imgMin, float innerSize, float gapPx, float s) {
    LobbyCompassGeom g;
    if (!lobbyCompassGeom(cs, effHostMap, imgMin, innerSize, gapPx, s, &g)) return false;

    /* No backing disc: the letters carry their own shadow. */
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImFont *font   = ImGui::GetFont();
    float fsz      = ImGui::GetFontSize() * 0.85f;
    dl->AddCircleFilled(g.c, (2.0f * s < 1.5f) ? 1.5f : 2.0f * s,
                        IM_COL32(255, 255, 255, 190), 12);

    for (int i = 0; i < 4; i++) {
        int  axis = lobbySideAxisOfSide(kCompassSides[i]);
        bool set  = (axis == g.setAxis);
        bool hot  = (axis == g.hovAxis);
        /* A set axis wears the colour of the team that holds that side;
         * a hovered one is bright white; everything else is dimmed. */
        ImU32 col = IM_COL32(255, 255, 255, hot ? 255 : 130);
        if (set) {
            int owner = (lobbyTeamSide(cs, g.teamA) == kCompassSides[i]) ? g.teamA : g.teamB;
            col = lobbyCompassTeamColor(cs, owner);
            if (!hot) {
                col = (col & ~(0xFFu << IM_COL32_A_SHIFT))
                    | (200u << IM_COL32_A_SHIFT);
            }
        }
        dl->AddLine(g.c, g.gp[i], col, ((set || hot) ? 2.0f : 1.0f) * s);
        char letter[16];
        SDL_strlcpy(letter, langGetText(lobbySideCompassId(kCompassSides[i])),
                    sizeof(letter));
        ImVec2 ts = font->CalcTextSizeA(fsz, FLT_MAX, 0.0f, letter);
        ImVec2 tp(g.gp[i].x - ts.x * 0.5f, g.gp[i].y - ts.y * 0.5f);
        dl->AddText(font, fsz, ImVec2(tp.x + 1.0f, tp.y + 1.0f),
                    IM_COL32(0, 0, 0, 205), letter);
        dl->AddText(font, fsz, tp, col, letter);
    }

    if (g.hovAxis == LOBBY_SIDE_AXIS_NONE) return g.overRose;

    /* Hover text: the axis, and -- when it is not the one already set --
     * which team takes which side of it. */
    MessageArgs args = {};
    char axisLbl[24];
    lobbyCompassAxisLabel(g.hovAxis, axisLbl, sizeof(axisLbl));
    SDL_strlcpy(args.string1, axisLbl, sizeof(args.string1));
    if (g.hovAxis == g.setAxis) {
        ImGui::SetTooltip("%s",
            langGetTextFmt(STR_DLGLOBBY_TOOLTIP_COMPASS_CLEAR, &args));
    } else {
        BYTE sa = START_SIDE_ANY, sb = START_SIDE_ANY;
        lobbySideAxisSides(g.hovAxis, &sa, &sb);
        lobbyTeamLabel(cs, g.teamA, args.playerName, sizeof(args.playerName));
        lobbyTeamLabel(cs, g.teamB, args.otherName, sizeof(args.otherName));
        SDL_strlcpy(args.string2, langGetText(lobbySideNameId(sa)),
                    sizeof(args.string2));
        SDL_strlcpy(args.string3, langGetText(lobbySideNameId(sb)),
                    sizeof(args.string3));
        ImGui::SetTooltip("%s",
            langGetTextFmt(STR_DLGLOBBY_TOOLTIP_COMPASS_SET, &args));
    }
    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        BYTE sa = START_SIDE_ANY, sb = START_SIDE_ANY;
        if (lobbySideAxisClick(g.hovAxis, g.setAxis, &sa, &sb)) {
            lobbySendTeamSide(cs, (uint8_t)g.teamA, sa);
            lobbySendTeamSide(cs, (uint8_t)g.teamB, sb);
        }
    }
    return true;
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
    /* The viewer's team, whose side decides which starts draw dimmed. A
     * spectator has none, so nothing is off-side for it. */
    const int myTeam = spectator ? 0 : lobbySlotTeam(cs, myPlayerNum);

    /* Pass 1: every start's holders, plus one flat name list for the
     * prefix disambiguation — the prefixes are computed against all the
     * holder names on the map, not just the ones sharing a start, so a
     * given player's initial reads the same wherever it appears. A start
     * several players share lists them all (nameIdx holds their entries in
     * the flat list, in slot order). */
    const char *holderNames[MAX_TANKS] = {0};
    int nameIdx[MAX_STARTS + 1][MAX_TANKS];
    int nNames[MAX_STARTS + 1];
    int nHolders = 0;
    for (int i = 1; i <= (int)s_mapPreview.startCount; i++) {
        int holders[MAX_TANKS];
        int n = lobbyStartHolders(cs, i, holders, MAX_TANKS);
        nNames[i] = 0;
        for (int h = 0; h < n && nHolders < MAX_TANKS; h++) {
            const char *nm = clientSimGetLobbySlot(cs, (BYTE)holders[h])->playerName;
            if (!nm || !nm[0]) continue;
            nameIdx[i][nNames[i]++] = nHolders;
            holderNames[nHolders++] = nm;
        }
    }

    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImFont *font   = ImGui::GetFont();
    float fsz      = ImGui::GetFontSize() * 0.85f;
    float tilePx   = (spanX > 0.0f) ? (previewSize / spanX) : 1.0f;

    for (int i = 1; i <= (int)s_mapPreview.startCount; i++) {
        if (s_mapPreview.startCompassId[i] == 0) continue;  /* not on the map */
        /* Start map-square centre -> displayed image pixel. */
        float fx = imgMin.x + (((float)s_mapPreview.startMapX[i] + 0.5f - bx0) / spanX) * previewSize;
        float fy = imgMin.y + (((float)s_mapPreview.startMapY[i] + 0.5f - by0) / spanY) * previewSize;
        if (fx < imgMin.x || fx > imgMin.x + previewSize ||
            fy < imgMin.y || fy > imgMin.y + previewSize) continue;

        /* Label: "#<n>" alone, or "#<n> <initials>" when claimed (the
         * disambiguating prefixes to the right of the number). Several
         * holders are joined with a comma — "#3 C, M". */
        char buf[96];
        if (nNames[i] > 0) {
            char who[64];
            lobbyStartHolderPrefixLabel(holderNames, nHolders,
                                        nameIdx[i], nNames[i], who, sizeof(who));
            SDL_snprintf(buf, sizeof(buf), "#%d %s", i, who);
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
         * red, unclaimed white. A start the viewer's own team side rejects
         * drops to half alpha, matching its dimmed dot in the texture. */
        ImU32 txtCol = IM_COL32(255, 255, 255, 255);
        ImU32 shadow = IM_COL32(0, 0, 0, 205);
        if (nNames[i] > 0) {
            LobbyStartOwner o = lobbyStartClassifyShared(cs, i,
                                                         spectator ? -1 : myPlayerNum);
            if (o == LSO_ENEMY) txtCol = IM_COL32(235, 90, 90, 255);
            else                txtCol = IM_COL32(80, 255, 170, 255);
        }
        if (lobbyStartOffSide(cs, myTeam, lobbyStartSideMask(i))) {
            txtCol = lobbyStartDimColor(txtCol);
            shadow = lobbyStartDimColor(shadow);
        }
        dl->AddText(font, fsz, ImVec2(tp.x + 1.0f, tp.y + 1.0f), shadow, buf);
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
 * light-blue focus outline. Clicking a free start moves you there, and — with
 * shared starts on — so does clicking one others already hold, which joins
 * them rather than pushing anyone off; pressing a start you hold, or (host)
 * one somebody else holds, and dragging reassigns that player (a manual drag).
 * A start that the placed player's team side rejects — the dragged player
 * during a drag, the viewer otherwise — takes neither a click nor a drop
 * from a player who is not the host — the server would refuse the claim —
 * and shows the arrow cursor and a tooltip saying so; the host keeps every
 * action and is told which team the start is off-side for.
 * Returns true if it consumed the click so the caller skips the zoom popup. */
bool lobbyPreviewInteract(ClientSim *cs, int myPlayerNum, bool effHostMap,
                                 ImVec2 imgMin, float innerSize,
                                 int bx0, int by0, int bx1, int by1) {
    bool consumed = false;
    /* A spectator owns no slot: it can neither claim a free start nor drag a
     * claimed one. Bail before any click is interpreted as an action. */
    if (clientSimIsSpectator(cs)) return consumed;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 mp = ImGui::GetMousePos();
    bool hov = ImGui::IsItemHovered();   /* the map Image (last item) */
    /* The start under the cursor — the click target and the drop target
     * alike — and whether the team side of the player being placed rejects
     * it: the dragged player during a drag, the viewer otherwise. Only the
     * host may put anyone on such a start. */
    int  st          = lobbyPreviewStartAtScreen(imgMin, innerSize, bx0, by0, bx1, by1,
                                                 mp, 25.0f);
    int  subject     = (s_miniDragHolder >= 0) ? s_miniDragHolder : myPlayerNum;
    bool offSide     = st >= 1 && lobbyStartOffSide(cs, lobbySlotTeam(cs, subject),
                                                    lobbyStartSideMask(st));
    bool dropBlocked = offSide && !effHostMap;
    auto offSideTooltip = [&]() {
        char tip[192];
        lobbyStartOffSideTip(cs, subject, effHostMap, st, tip, sizeof(tip));
        ImGui::SetTooltip("%s", tip);
    };

    if (s_miniDragHolder >= 0) {
        consumed = true;
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            s_miniDragHolder = -1;
            s_miniDragJoin   = false;
            return consumed;
        }
        /* A press that may still be a join (host on a start somebody else
         * holds) shows nothing of the drag until the cursor actually
         * travels, so a plain click reads as a click and not as a grab. */
        ImVec2 ddNow = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
        bool dragging = !s_miniDragJoin ||
                        (ddNow.x * ddNow.x + ddNow.y * ddNow.y >= 16.0f);
        if (dragging) {
            ImGui::SetMouseCursor(dropBlocked ? ImGuiMouseCursor_Arrow
                                              : ImGuiMouseCursor_ResizeAll);
        }
        if (offSide) offSideTooltip();
        const ClientLobbySlot *ds = clientSimGetLobbySlot(cs, (BYTE)s_miniDragHolder);
        if (dragging && ds && ds->playerName[0]) {
            ImVec2 ts = ImGui::CalcTextSize(ds->playerName);
            ImVec2 p(mp.x + 12.0f, mp.y - ts.y * 0.5f);
            dl->AddRectFilled(ImVec2(p.x - 3, p.y - 1),
                              ImVec2(p.x + ts.x + 3, p.y + ts.y + 1),
                              IM_COL32(0, 0, 0, 200), 3.0f);
            dl->AddText(p, IM_COL32(255, 255, 255, 255), ds->playerName);
        }
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            /* The host's press on a start somebody else holds is a join
             * until it turns into a real drag: under 4 px of travel the
             * release puts the viewer on the start instead of moving the
             * holder off it. A drop onto a held start just joins it too —
             * the server no longer swaps anyone out. */
            ImVec2 dd = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
            bool moved = (dd.x * dd.x + dd.y * dd.y >= 16.0f);
            if (st >= 1 && !dropBlocked) {
                BYTE who = (s_miniDragJoin && !moved && myPlayerNum >= 0)
                               ? (BYTE)myPlayerNum : (BYTE)s_miniDragHolder;
                clientSimNetSendLobbyClaimStart(cs, who, (BYTE)st);
            }
            s_miniDragHolder = -1;
            s_miniDragJoin   = false;
        }
        return consumed;
    }

    if (!hov) return consumed;
    /* Every connected slot on the start under the cursor: the first of them
     * is who the host drags, and whether the viewer is among them decides
     * between "drag me" and "join them". */
    int holders[MAX_TANKS];
    int nHold  = (st >= 1) ? lobbyStartHolders(cs, st, holders, MAX_TANKS) : 0;
    int holder = (nHold > 0) ? holders[0] : -1;
    bool iHold = false;
    for (int h = 0; h < nHold; h++) {
        if (holders[h] == myPlayerNum) iHold = true;
    }
    /* Nothing to do on an off-side start unless you are the host or already
     * hold it — your own claim can still be dragged away. */
    bool blocked = dropBlocked && !iHold;
    ImGui::SetMouseCursor(blocked ? ImGuiMouseCursor_Arrow : ImGuiMouseCursor_Hand);
    if (offSide) offSideTooltip();
    /* On press: a free start under the cursor → claim it for yourself; a
     * start you hold → begin a manual drag-to-move. With shared starts on,
     * a start somebody else holds is one you may join: a non-host joins on
     * the press, and for the host the press begins a potential drag of the
     * first holder that a release without movement turns into a join.
     * Every one of those consumes the click so the zoom popup doesn't open,
     * as does a press on a start you cannot take, so that it does nothing
     * at all. */
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (blocked) {
            consumed = true;
        } else if (st >= 1 && holder < 0 && myPlayerNum >= 0) {
            clientSimNetSendLobbyClaimStart(cs, (BYTE)myPlayerNum, (BYTE)st);
            consumed = true;
        } else if (iHold) {
            s_miniDragHolder = myPlayerNum;
            s_miniDragJoin   = false;
            consumed = true;
        } else if (holder >= 0 && effHostMap) {
            s_miniDragHolder = holder;
            s_miniDragJoin   = lobbySharedStartsEnabled();
            consumed = true;
        } else if (holder >= 0 && lobbySharedStartsEnabled() && myPlayerNum >= 0) {
            clientSimNetSendLobbyClaimStart(cs, (BYTE)myPlayerNum, (BYTE)st);
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
