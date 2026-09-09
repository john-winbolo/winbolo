/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * Name:          ping_overlay.cpp
 * Purpose:       See ping_overlay.h.
 *
 * Coordinate spaces, because three of them meet here:
 *
 *   WORLD    - what the wire and the sim carry. 256 units to
 *              a map square, so a ping keeps its sub-square
 *              position instead of snapping to a tile.
 *   game     - the renderer's logical space, the one the
 *              window->game transform hands back. The 15x15
 *              view sits at a fixed offset inside it.
 *   window   - actual on-screen pixels, which is what ImGui
 *              draws in.
 *
 * The pie menu lives entirely in window pixels (it is drawn
 * at the cursor and never moves with the map). Received
 * pings are stored in WORLD and converted out to window
 * pixels every frame, so a marker stays over the same ground
 * while the view scrolls under it.
 *********************************************************/

#include "ping_overlay.h"

#include <math.h>

#include "imgui.h"

extern "C" {
#include "sdl3draw.h"
#include "sdl3imgui.h"                    /* sdl3ImguiGetUiScale */
#include "../winbolo.h"                   /* windowGetKeys */
#include "../input.h"                     /* keyItems */
#include "../ping_kinds.h"
#include "../lang.h"                     /* langGetText — the slice names */
#include "ping_binding.h"
#include "ping_icons.h"
#include "ping_pie.h"
#include "ping_edge.h"
#include "client_sim.h"
#include "client_net.h"
}

/* ------------------------------------------------------------------
 * Icons — loaded once per renderer by ping_icons.c, which the log
 * viewer's replay draw shares.
 * ------------------------------------------------------------------ */

void pingOverlayInit(SDL_Renderer *renderer) {
    pingIconsInit(renderer);
}

void pingOverlayShutdown(void) {
    pingIconsShutdown();
}

/* ------------------------------------------------------------------
 * Menu state
 * ------------------------------------------------------------------ */

static bool  s_open        = false;
static int   s_triggerCode = 0;      /* the key/mouse code that opened it */
static float s_anchorX     = 0.0f;   /* window pixels: where the press landed */
static float s_anchorY     = 0.0f;
static float s_cursorX     = 0.0f;   /* window pixels: where the cursor is now */
static float s_cursorY     = 0.0f;
static uint16_t s_worldX   = 0;      /* the ground under the press */
static uint16_t s_worldY   = 0;

bool pingOverlayIsMenuOpen(void) { return s_open; }

/* Close with nothing sent. Not exported: every way out of the menu is
   decided in this file. */
static void pingOverlayCancel(void) {
    s_open = false;
    s_triggerCode = 0;
}

/* ------------------------------------------------------------------
 * Coordinate helpers
 * ------------------------------------------------------------------ */

/* The main view in window pixels, plus the window-pixel size of one map
 * square. Everything the drawer and the hit test need in one call. */
typedef struct {
    float x, y, w, h;    /* the 15x15 view, window pixels */
    float tileW, tileH;  /* one map square, window pixels */
    float scale;         /* window pixels per game logical pixel */
} PingViewRect;

static bool viewRect(PingViewRect *out) {
    float gx, gy, gw, gh, gtw, gth;
    float wx0, wy0, wx1, wy1;
    if (!sdl3DrawGetMainViewGameRect(&gx, &gy, &gw, &gh, &gtw, &gth)) return false;
    if (!sdl3DrawGameToWindowCoords(gx, gy, &wx0, &wy0)) return false;
    if (!sdl3DrawGameToWindowCoords(gx + gw, gy + gh, &wx1, &wy1)) return false;
    if (wx1 <= wx0 || wy1 <= wy0) return false;
    out->x = wx0;
    out->y = wy0;
    out->w = wx1 - wx0;
    out->h = wy1 - wy0;
    out->scale = out->w / gw;
    out->tileW = gtw * out->scale;
    out->tileH = gth * out->scale;
    return true;
}

/* Where in the view a WORLD position lands, in window pixels.
 *
 * The view shows map squares xOffset+1 .. xOffset+MAIN_SCREEN_SIZE_X, and
 * scrolls sub-square by clientSimGetSubPos* (0..255 of a square) — the same
 * two numbers the cursor hit test in cursor.c runs backwards. */
static void worldToWindow(struct ClientSim *cs, const PingViewRect *vr,
                          uint16_t worldX, uint16_t worldY,
                          float *outX, float *outY) {
    float squaresX = (float)worldX / 256.0f - (float)(clientSimGetXOffset(cs) + 1);
    float squaresY = (float)worldY / 256.0f - (float)(clientSimGetYOffset(cs) + 1);
    float edgeX = (float)clientSimGetSubPosX(cs) * vr->tileW / 256.0f;
    float edgeY = (float)clientSimGetSubPosY(cs) * vr->tileH / 256.0f;
    *outX = vr->x + squaresX * vr->tileW - edgeX;
    *outY = vr->y + squaresY * vr->tileH - edgeY;
}

/* The inverse: which ground a window pixel is over. Returns false when the
 * point is outside the view or off the map. */
static bool windowToWorld(struct ClientSim *cs, const PingViewRect *vr,
                          float winX, float winY,
                          uint16_t *outX, uint16_t *outY) {
    float edgeX, edgeY, sqX, sqY;
    if (winX < vr->x || winX >= vr->x + vr->w) return false;
    if (winY < vr->y || winY >= vr->y + vr->h) return false;
    edgeX = (float)clientSimGetSubPosX(cs) * vr->tileW / 256.0f;
    edgeY = (float)clientSimGetSubPosY(cs) * vr->tileH / 256.0f;
    sqX = (winX - vr->x + edgeX) / vr->tileW + (float)(clientSimGetXOffset(cs) + 1);
    sqY = (winY - vr->y + edgeY) / vr->tileH + (float)(clientSimGetYOffset(cs) + 1);
    if (sqX < 0.0f || sqY < 0.0f) return false;
    {
        float wx = sqX * 256.0f;
        float wy = sqY * 256.0f;
        if (wx > 65535.0f || wy > 65535.0f) return false;
        *outX = (uint16_t)wx;
        *outY = (uint16_t)wy;
    }
    return true;
}

/* Which modifier bits are down right now, in pingBinding's vocabulary. */
static int currentMods(void) {
    SDL_Keymod km = SDL_GetModState();
    int mods = 0;
    if (km & SDL_KMOD_CTRL)  mods |= PING_BIND_MOD_CTRL;
    if (km & SDL_KMOD_ALT)   mods |= PING_BIND_MOD_ALT;
    if (km & SDL_KMOD_SHIFT) mods |= PING_BIND_MOD_SHIFT;
    return mods;
}

/* Can this client ping at all right now? A spectator is read-only, the lobby
 * has no map to point at, and a client that is not in a running game has
 * nothing to send over. */
static bool canPing(struct ClientSim *cs) {
    if (cs == nullptr) return false;
    if (clientSimIsSpectator(cs)) return false;
    if (clientSimIsInLobby(cs)) return false;
    return clientSimGetNetStatus(cs) == netRunning;
}

/* ------------------------------------------------------------------
 * Event handling
 * ------------------------------------------------------------------ */

static void sendSelected(struct ClientSim *cs) {
    float scale = sdl3ImguiGetUiScale();
    if (scale <= 0.0f) scale = 1.0f;
    unsigned char kind = pingPieKindAt(s_cursorX - s_anchorX,
                                       s_cursorY - s_anchorY,
                                       PING_PIE_DEADZONE_PX * scale);
    clientSimNetSendPing(cs, kind, s_worldX, s_worldY);
}

bool pingOverlayHandleEvent(struct ClientSim *cs, const SDL_Event *ev) {
    keyItems keys;
    if (ev == nullptr) return false;

    /* The game moving out from under an open menu — the round ending, a
       disconnect, a return to the lobby — takes the release with it, so drop
       the menu rather than leave it armed to eat the next click. */
    if (s_open && !canPing(cs)) pingOverlayCancel();

    /* While the menu is open it owns the pointer and the trigger's release,
       whatever else the game would have done with them. */
    if (s_open) {
        switch (ev->type) {
        case SDL_EVENT_MOUSE_MOTION:
            s_cursorX = ev->motion.x;
            s_cursorY = ev->motion.y;
            return true;
        case SDL_EVENT_KEY_DOWN:
            /* Escape abandons the menu outright — it is the one way out that
               sends nothing at all. Moving back to the centre is not a
               cancel: that selects the standard ping, on release. */
            if (ev->key.scancode == SDL_SCANCODE_ESCAPE) {
                pingOverlayCancel();
                return true;
            }
            return false;
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (pingBindingMouseCode(ev->button.button) == s_triggerCode) {
                s_cursorX = ev->button.x;
                s_cursorY = ev->button.y;
                sendSelected(cs);
                pingOverlayCancel();
                return true;
            }
            return false;
        case SDL_EVENT_KEY_UP:
            if ((int)ev->key.scancode == s_triggerCode) {
                sendSelected(cs);
                pingOverlayCancel();
                return true;
            }
            return false;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            /* The release will never arrive, so holding the menu open would
               strand it over the game. */
            pingOverlayCancel();
            return false;
        default:
            return false;
        }
    }

    /* Closed: look for the chord that opens it. */
    {
        int code = 0;
        float px = 0.0f, py = 0.0f;
        PingViewRect vr;

        if (ev->type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
            code = pingBindingMouseCode(ev->button.button);
            px = ev->button.x;
            py = ev->button.y;
        } else if (ev->type == SDL_EVENT_KEY_DOWN && !ev->key.repeat) {
            float mx = 0.0f, my = 0.0f;
            code = (int)ev->key.scancode;
            SDL_GetMouseState(&mx, &my);
            px = mx;
            py = my;
        } else {
            return false;
        }
        if (code == 0) return false;

        windowGetKeys(&keys);
        if (pingBindingMatchAny(keys.kiPing, PING_BIND_SLOTS, code,
                                currentMods()) < 0) {
            return false;
        }
        if (!canPing(cs)) return false;
        if (!viewRect(&vr)) return false;
        /* The ping goes where the cursor was when the menu opened, so a press
           that started outside the map has nothing to point at. */
        if (!windowToWorld(cs, &vr, px, py, &s_worldX, &s_worldY)) return false;

        s_open        = true;
        s_triggerCode = code;
        s_anchorX = s_cursorX = px;
        s_anchorY = s_cursorY = py;
        return true;
    }
}

/* ------------------------------------------------------------------
 * Drawing
 * ------------------------------------------------------------------ */

static ImU32 kindColour(unsigned char kind, float alpha) {
    const PingKindStyle *st = pingKindStyle(kind);
    int a = (int)(alpha * 255.0f + 0.5f);
    if (a < 0) a = 0;
    if (a > 255) a = 255;
    return IM_COL32(st->r, st->g, st->b, a);
}

/* One icon, tinted to its kind, centred on (cx, cy). Falls back to a filled
 * disc when the icon file did not load, so a marker is never invisible. */
static void drawIcon(ImDrawList *dl, unsigned char kind,
                     float cx, float cy, float size, float alpha) {
    ImU32 col = kindColour(kind, alpha);
    SDL_Texture *tex = pingIconTexture(kind);
    float h = size * 0.5f;
    if (tex) {
        dl->AddImage((ImTextureID)tex, ImVec2(cx - h, cy - h),
                     ImVec2(cx + h, cy + h), ImVec2(0, 0), ImVec2(1, 1), col);
    } else {
        dl->AddCircleFilled(ImVec2(cx, cy), h * 0.5f, col, 16);
    }
}

/* Is the key or button that opened the menu still down? SDL's live device
 * state rather than a remembered flag, so it is right even when the event
 * that would have cleared the flag was never delivered. */
static bool pingTriggerHeld(void) {
    if (s_triggerCode == 0) return false;
    if (s_triggerCode >= PING_BIND_MOUSE_BASE) {
        int button = s_triggerCode - PING_BIND_MOUSE_BASE;
        SDL_MouseButtonFlags held = SDL_GetMouseState(nullptr, nullptr);
        return (held & SDL_BUTTON_MASK(button)) != 0;
    }
    {
        int numKeys = 0;
        const bool *keyState = SDL_GetKeyboardState(&numKeys);
        if (keyState == nullptr || s_triggerCode >= numKeys) return false;
        return keyState[s_triggerCode];
    }
}

/* The pie: a dark plate so the icons read over any terrain, the slice under
 * the cursor lit in its own colour, and the centre dot for the standard
 * ping. */
static void drawMenu(void) {
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    float scale = sdl3ImguiGetUiScale();
    float radius, dead, iconPx, dx, dy;
    int   hot;

    if (scale <= 0.0f) scale = 1.0f;
    radius = PING_PIE_RADIUS_PX * scale;
    dead   = PING_PIE_DEADZONE_PX * scale;
    iconPx = PING_PIE_ICON_PX * scale;

    dx = s_cursorX - s_anchorX;
    dy = s_cursorY - s_anchorY;
    hot = pingPieSliceAt(dx, dy, dead);

    dl->AddCircleFilled(ImVec2(s_anchorX, s_anchorY), radius,
                        IM_COL32(0, 0, 0, 150), 48);
    dl->AddCircle(ImVec2(s_anchorX, s_anchorY), radius,
                  IM_COL32(255, 255, 255, 70), 48, 2.0f * scale);

    /* Slice separators, drawn on the boundaries rather than the centre lines
       so each icon sits in the middle of its own wedge. */
    for (int i = 0; i < PING_PIE_SLICES; i++) {
        float a = pingPieSliceAngle(i) + (float)(M_PI / PING_PIE_SLICES);
        float sx = sinf(a), sy = -cosf(a);
        dl->AddLine(ImVec2(s_anchorX + sx * dead, s_anchorY + sy * dead),
                    ImVec2(s_anchorX + sx * radius, s_anchorY + sy * radius),
                    IM_COL32(255, 255, 255, 50), 1.0f * scale);
    }

    for (int i = 0; i < PING_PIE_SLICES; i++) {
        unsigned char kind = kPingPieSlices[i];
        float ox, oy;
        bool  lit = (hot == i);
        pingPieIconOffset(i, radius, &ox, &oy);
        if (lit) {
            dl->AddCircleFilled(ImVec2(s_anchorX + ox, s_anchorY + oy),
                                iconPx * 0.75f, kindColour(kind, 0.30f), 24);
        }
        drawIcon(dl, kind, s_anchorX + ox, s_anchorY + oy, iconPx,
                 lit ? 1.0f : 0.75f);
    }

    /* The centre: the standard ping, lit whenever the cursor is in the dead
       zone — which is also where a quick tap leaves it. */
    {
        bool lit = (hot < 0);
        dl->AddCircleFilled(ImVec2(s_anchorX, s_anchorY), dead,
                            IM_COL32(0, 0, 0, 170), 24);
        if (lit) {
            dl->AddCircleFilled(ImVec2(s_anchorX, s_anchorY), dead,
                                kindColour(PING_KIND_STANDARD, 0.25f), 24);
        }
        drawIcon(dl, PING_KIND_STANDARD, s_anchorX, s_anchorY,
                 dead * 1.2f, lit ? 1.0f : 0.6f);
    }

    /* The name of what is currently selected, under the pie. Only the one —
       six labels round a ring at this size is a wall of text, and the player
       only needs to know what releasing now would send. */
    {
        unsigned char kind = (hot < 0) ? (unsigned char)PING_KIND_STANDARD
                                       : kPingPieSlices[hot];
        const char *name = langGetText(pingKindNameId(kind));
        ImVec2 sz = ImGui::CalcTextSize(name);
        ImVec2 at(s_anchorX - sz.x * 0.5f, s_anchorY + radius + 6.0f * scale);
        dl->AddRectFilled(ImVec2(at.x - 6.0f * scale, at.y - 2.0f * scale),
                          ImVec2(at.x + sz.x + 6.0f * scale,
                                 at.y + sz.y + 2.0f * scale),
                          IM_COL32(0, 0, 0, 170), 3.0f * scale);
        dl->AddText(at, kindColour(kind, 1.0f), name);
    }
}

/* Pings inside the view are not drawn here: the game frame paints them on
 * the ground (sdl3draw.c, through ping_marker.c) between the terrain and the
 * sprites, so tanks, shells and builders stay on top. This overlay handles
 * only what belongs above everything -- the pie menu, and the edge marker
 * for a ping the view cannot show. */

void pingOverlayDraw(struct ClientSim *cs) {
    ClientPing    pings[MAX_CLIENT_PINGS];
    PingViewRect  vr;
    ImDrawList   *dl;
    uint32_t      now;
    int           n, i;
    float         tankX, tankY;
    float         tankMapX = 0.0f, tankMapY = 0.0f;
    bool          haveTank;

    /* A release that never arrived as an event. Something above the game can
       swallow the button-up — a modal opening under the player's finger, a
       focus hand-off — and the menu would then sit open over the view with no
       way to close it. If the trigger is no longer physically down, take that
       as the release it was: the player let go, and letting go is what sends
       the ping they had selected. */
    if (s_open && !pingTriggerHeld()) {
        sendSelected(cs);
        pingOverlayCancel();
    }

    if (s_open) drawMenu();

    if (cs == nullptr) return;
    if (!viewRect(&vr)) return;

    now = SDL_GetTicks();
    n = clientSimGetPings(cs, now, pings, MAX_CLIENT_PINGS);
    if (n <= 0) return;

    dl = ImGui::GetForegroundDrawList();

    /* The edge marker points from the viewer's tank. A dead or not-yet-placed
       tank has no position, in which case the centre of the view — which is
       where the camera is anyway — stands in for it. */
    haveTank = clientSimGetMyTankMapPosF(cs, &tankMapX, &tankMapY);
    if (haveTank) {
        worldToWindow(cs, &vr,
                      (uint16_t)(tankMapX * 256.0f),
                      (uint16_t)(tankMapY * 256.0f), &tankX, &tankY);
    } else {
        tankX = vr.x + vr.w * 0.5f;
        tankY = vr.y + vr.h * 0.5f;
    }

    for (i = 0; i < n; i++) {
        int   ageMs = (int)(now - pings[i].recvMs);
        float alpha = pingDisplayAlpha(ageMs);
        float px, py;
        if (alpha <= 0.0f) continue;
        /* Anchor on the centre of the map square the ping landed in, so the
           marker names one tile rather than a point between tiles. */
        worldToWindow(cs, &vr,
                      (uint16_t)((pings[i].worldX & 0xFF00u) | 0x80u),
                      (uint16_t)((pings[i].worldY & 0xFF00u) | 0x80u), &px, &py);
        if (px >= vr.x && px <= vr.x + vr.w && py >= vr.y && py <= vr.y + vr.h) {
            continue;   /* on the ground, drawn by the game frame */
        }
        {
            PingEdgeMarker m;
            float thick  = PING_EDGE_THICKNESS_PX * vr.scale;
            float length = PING_EDGE_LENGTH_PX * vr.scale;
            float iconPx, ix, iy;
            if (!pingEdgeMarker(vr.x, vr.y, vr.w, vr.h, tankX, tankY, px, py,
                                length, &m)) {
                continue;
            }
            /* Pulled half a thickness inside the border so the whole bar is
               on the game view rather than half over the chrome. */
            if (m.side == PING_EDGE_LEFT)   { m.x0 += thick * 0.5f; m.x1 += thick * 0.5f; }
            if (m.side == PING_EDGE_RIGHT)  { m.x0 -= thick * 0.5f; m.x1 -= thick * 0.5f; }
            if (m.side == PING_EDGE_TOP)    { m.y0 += thick * 0.5f; m.y1 += thick * 0.5f; }
            if (m.side == PING_EDGE_BOTTOM) { m.y0 -= thick * 0.5f; m.y1 -= thick * 0.5f; }
            dl->AddLine(ImVec2(m.x0, m.y0), ImVec2(m.x1, m.y1),
                        IM_COL32(0, 0, 0, (int)(140 * alpha)), thick + 2.0f);
            dl->AddLine(ImVec2(m.x0, m.y0), ImVec2(m.x1, m.y1),
                        kindColour(pings[i].kind, alpha), thick);

            /* The kind's icon just inside the bar, so the colour alone does
               not have to carry which ping it is. One tile across, never
               smaller than legible, centred on the bar and stepped inward
               from it by its own half-size plus a little air. */
            iconPx = vr.tileW;
            if (iconPx < 18.0f) iconPx = 18.0f;
            ix = (m.x0 + m.x1) * 0.5f;
            iy = (m.y0 + m.y1) * 0.5f;
            {
                float step = thick * 0.5f + iconPx * 0.5f + 3.0f * vr.scale;
                if (m.side == PING_EDGE_LEFT)   ix += step;
                if (m.side == PING_EDGE_RIGHT)  ix -= step;
                if (m.side == PING_EDGE_TOP)    iy += step;
                if (m.side == PING_EDGE_BOTTOM) iy -= step;
            }
            dl->AddCircleFilled(ImVec2(ix, iy), iconPx * 0.6f,
                                IM_COL32(0, 0, 0, (int)(150 * alpha)), 24);
            drawIcon(dl, pings[i].kind, ix, iy, iconPx, alpha);
        }
    }
}
