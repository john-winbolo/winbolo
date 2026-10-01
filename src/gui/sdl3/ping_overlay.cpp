/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
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
 *   game     - the game's own layout space, the one the
 *              window->game transform hands back. The 15x15
 *              view sits at a fixed offset inside it.
 *   screen   - the renderer's coordinates: what ImGui draws
 *              in and reports the mouse in. Equal to window
 *              pixels until a logical presentation is set,
 *              when SDL scales that space up to the window
 *              (see the invariant in ping_overlay.h).
 *
 * The pie menu lives entirely in screen coordinates (it is
 * drawn at the cursor and never moves with the map). Received
 * pings are stored in WORLD and converted out to screen
 * coordinates every frame, so a marker stays over the same
 * ground while the view scrolls under it.
 *
 * Two maps can be the one on screen, and the conversion is
 * not the same for both:
 *
 *   the classic 15x15 view - xOffset/yOffset plus the
 *     sub-square scroll, the arithmetic in classicWorldToScreen.
 *   the map overview filling the window - its own camera,
 *     through overviewCameraScreenToWorld and
 *     overviewCameraWorldToScreen, which work in whole map
 *     squares one way and fractional ones the other.
 *
 * Which of them is up is recorded by the drawing side each
 * frame (pingOverlaySetClassicSurface /
 * pingOverlaySetOverviewSurface) and read back here, both by
 * the event hook deciding where a press landed and by the
 * drawer placing the edge bars.
 *********************************************************/

#include "ping_overlay.h"

#include <float.h>   /* FLT_MAX — "no wrap" for ImFont::CalcTextSizeA */
#include <math.h>
#include <string.h>

#include "imgui.h"
#include "overview_camera.h"   /* the overview's own screen <-> world maths */

extern "C" {
#include "sdl3draw.h"
#include "sdl3imgui.h"                    /* sdl3ImguiGetUiScale */
#include "../winbolo.h"                   /* windowGetKeys */
#include "../input.h"                     /* keyItems */
#include "../ping_kinds.h"
#include "../lang.h"                     /* langGetText — the slice names */
#include "ping_binding.h"
#include "ping_icons.h"
#include "input_gamepad.h"   /* the pad's own ping bindings and aim */
#include "build_cursor.h"    /* the square a pad ping lands on */
#include "viewport_types.h"  /* MAIN_SCREEN_SIZE_X / _Y */
#include "ping_pie.h"
#include "ping_edge.h"
#include "client_sim.h"
#include "client_net.h"
}

/* ------------------------------------------------------------------
 * Icons — loaded once per renderer by ping_icons.c, which the log
 * viewer's replay draw shares.
 * ------------------------------------------------------------------ */

/* The renderer everything here is drawn through. Kept for the two questions
   only it can answer: which window the pie belongs to, and where a point SDL
   reports in window coordinates lands in the renderer's own space. */
static SDL_Renderer *s_renderer = nullptr;

void pingOverlayInit(SDL_Renderer *renderer) {
    s_renderer = renderer;
    pingIconsInit(renderer);
}

void pingOverlayShutdown(void) {
    s_renderer = nullptr;
    pingIconsShutdown();
}

/* ------------------------------------------------------------------
 * Menu state
 * ------------------------------------------------------------------ */

static bool  s_open        = false;
static int   s_triggerCode = 0;      /* the key/mouse code that opened it */
static float s_anchorX     = 0.0f;   /* screen coords: where the press landed */
static float s_anchorY     = 0.0f;
static float s_cursorX     = 0.0f;   /* screen coords: where the cursor is now */
static float s_cursorY     = 0.0f;
static uint16_t s_worldX   = 0;      /* the ground under the press */
static uint16_t s_worldY   = 0;
/* The menu was opened from the controller rather than from the pointer. It
   has no trigger code to watch and no cursor of its own: the aim comes from
   the pad's stick or d-pad and the release is the binding going up. */
static bool  s_padOpen     = false;

bool pingOverlayIsMenuOpen(void) { return s_open; }

/* Close with nothing sent. Not exported: every way out of the menu is
   decided in this file. */
static void pingOverlayCancel(void) {
    s_open = false;
    s_padOpen = false;
    s_triggerCode = 0;
}

/* ------------------------------------------------------------------
 * Coordinate helpers
 * ------------------------------------------------------------------ */

/* A point SDL reports in window coordinates — SDL_GetMouseState's, or any
 * event that has not been through SDL_ConvertEventToRenderCoordinates — moved
 * into the screen space everything here works in. The identity until a
 * logical presentation is set. */
static void windowToScreen(float wx, float wy, float *sx, float *sy) {
    if (s_renderer == nullptr ||
        !SDL_RenderCoordinatesFromWindow(s_renderer, wx, wy, sx, sy)) {
        *sx = wx;
        *sy = wy;
    }
}

/* The main view in screen coordinates, plus the size of one map square there.
 * Everything the drawer and the hit test need in one call. */
typedef struct {
    float x, y, w, h;    /* the 15x15 view, screen coordinates */
    float tileW, tileH;  /* one map square, screen coordinates */
    float scale;         /* screen units per game logical pixel */
} PingViewRect;

static bool viewRect(PingViewRect *out) {
    float gx, gy, gw, gh, gtw, gth;
    float wx0, wy0, wx1, wy1;
    if (!sdl3DrawGetMainViewGameRect(&gx, &gy, &gw, &gh, &gtw, &gth)) return false;
    /* Render coordinates rather than window pixels: the pie is painted on
       ImGui's draw list and hit-tested against events ImGui has already
       converted, and under a logical presentation those are not the same
       space. */
    if (!sdl3DrawGameToRenderCoords(gx, gy, &wx0, &wy0)) return false;
    if (!sdl3DrawGameToRenderCoords(gx + gw, gy + gh, &wx1, &wy1)) return false;
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

/* Where in the view a WORLD position lands, in screen coordinates.
 *
 * The view shows map squares xOffset+1 .. xOffset+MAIN_SCREEN_SIZE_X, and
 * scrolls sub-square by clientSimGetSubPos* (0..255 of a square) — the same
 * two numbers the cursor hit test in cursor.c runs backwards, and the same
 * arithmetic sdl3draw.c places the on-the-ground markers with. Both of those
 * work in game coordinates; this one works in a rect that is that space
 * scaled and shifted as a whole, which every term here follows. */
static void classicWorldToScreen(struct ClientSim *cs, const PingViewRect *vr,
                                 uint16_t worldX, uint16_t worldY,
                                 float *outX, float *outY) {
    float squaresX = (float)worldX / 256.0f - (float)(clientSimGetXOffset(cs) + 1);
    float squaresY = (float)worldY / 256.0f - (float)(clientSimGetYOffset(cs) + 1);
    float edgeX = (float)clientSimGetSubPosX(cs) * vr->tileW / 256.0f;
    float edgeY = (float)clientSimGetSubPosY(cs) * vr->tileH / 256.0f;
    *outX = vr->x + squaresX * vr->tileW - edgeX;
    *outY = vr->y + squaresY * vr->tileH - edgeY;
}

/* The inverse: which ground a screen point is over. Returns false when the
 * point is outside the view or off the map. */
static bool classicScreenToWorld(struct ClientSim *cs, const PingViewRect *vr,
                                 float sx, float sy,
                                 uint16_t *outX, uint16_t *outY) {
    float edgeX, edgeY, sqX, sqY;
    if (sx < vr->x || sx >= vr->x + vr->w) return false;
    if (sy < vr->y || sy >= vr->y + vr->h) return false;
    edgeX = (float)clientSimGetSubPosX(cs) * vr->tileW / 256.0f;
    edgeY = (float)clientSimGetSubPosY(cs) * vr->tileH / 256.0f;
    sqX = (sx - vr->x + edgeX) / vr->tileW + (float)(clientSimGetXOffset(cs) + 1);
    sqY = (sy - vr->y + edgeY) / vr->tileH + (float)(clientSimGetYOffset(cs) + 1);
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

/* ------------------------------------------------------------------
 * The recorded map surface
 * ------------------------------------------------------------------ */

typedef enum {
    PING_SURFACE_NONE = 0,
    PING_SURFACE_CLASSIC,
    PING_SURFACE_OVERVIEW
} PingSurfaceKind;

static struct {
    PingSurfaceKind kind;
    float           x, y, w, h;   /* the map's rect on screen */
    bool            hoverable;    /* pointer on it, no panel over it */
    OverviewCamera  cam;          /* overview only: last frame's camera */
    int             viewW, viewH; /* overview only: the offscreen's size */
} s_surface;

void pingOverlayClearSurface(void) {
    s_surface.kind      = PING_SURFACE_NONE;
    s_surface.hoverable = false;
}

void pingOverlaySetClassicSurface(bool hoverable) {
    PingViewRect vr;
    if (!viewRect(&vr)) {
        pingOverlayClearSurface();
        return;
    }
    s_surface.kind      = PING_SURFACE_CLASSIC;
    s_surface.x         = vr.x;
    s_surface.y         = vr.y;
    s_surface.w         = vr.w;
    s_surface.h         = vr.h;
    s_surface.hoverable = hoverable;
}

void pingOverlaySetOverviewSurface(float x, float y, float w, float h,
                                   bool hoverable,
                                   const struct OverviewCamera *cam,
                                   int viewW, int viewH) {
    if (cam == nullptr || w <= 0.0f || h <= 0.0f || viewW <= 0 || viewH <= 0) {
        pingOverlayClearSurface();
        return;
    }
    s_surface.kind      = PING_SURFACE_OVERVIEW;
    s_surface.x         = x;
    s_surface.y         = y;
    s_surface.w         = w;
    s_surface.h         = h;
    s_surface.hoverable = hoverable;
    /* Copied rather than pointed at: the camera goes on moving — follow, a
       scroll, a drag — and the press has to land on the ground the player was
       looking at when they pressed. */
    s_surface.cam       = *cam;
    s_surface.viewW     = viewW;
    s_surface.viewH     = viewH;
}

/* The ground under a screen point, whichever map is up. False when there is
 * no map, when the pointer was over a panel last frame, when the point is
 * outside the map's rect, or when it is off the map itself. */
static bool pingSurfaceWorldAt(struct ClientSim *cs, float px, float py,
                               uint16_t *outX, uint16_t *outY) {
    if (!s_surface.hoverable) return false;
    if (px < s_surface.x || px >= s_surface.x + s_surface.w) return false;
    if (py < s_surface.y || py >= s_surface.y + s_surface.h) return false;

    if (s_surface.kind == PING_SURFACE_OVERVIEW) {
        int mapX = 0, mapY = 0;
        /* The overview names whole squares, so the ping is anchored on the
           square's centre rather than on a sub-square point the picture never
           offered the player. */
        if (!overviewCameraScreenToWorld(&s_surface.cam,
                                         s_surface.viewW, s_surface.viewH,
                                         px - s_surface.x, py - s_surface.y,
                                         &mapX, &mapY)) {
            return false;
        }
        *outX = (uint16_t)(mapX * 256 + 128);
        *outY = (uint16_t)(mapY * 256 + 128);
        return true;
    }
    if (s_surface.kind == PING_SURFACE_CLASSIC) {
        PingViewRect vr;
        if (!viewRect(&vr)) return false;
        return classicScreenToWorld(cs, &vr, px, py, outX, outY);
    }
    return false;
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

    /* A menu opened from the controller takes its aim from the pad, so
       pointer motion is none of its business — a hand resting on the mouse
       must not drag the selection away from where the stick is pointing.
       Escape still abandons it, the way it abandons the pointer's. */
    if (s_open && s_padOpen) {
        if (ev->type == SDL_EVENT_KEY_DOWN &&
            ev->key.scancode == SDL_SCANCODE_ESCAPE) {
            pingOverlayCancel();
            return true;
        }
        return false;
    }

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
        int code = 0, mods = 0, direct = -1;
        float px = 0.0f, py = 0.0f;

        if (ev->type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
            code = pingBindingMouseCode(ev->button.button);
            px = ev->button.x;
            py = ev->button.y;
        } else if (ev->type == SDL_EVENT_KEY_DOWN && !ev->key.repeat) {
            float mx = 0.0f, my = 0.0f;
            /* The keyboard put this press in the game window; the pointer may
               be somewhere else entirely. SDL_GetMouseState answers against
               whichever window holds the POINTER, so with a pop-out under the
               cursor its pixels would be hit-tested against the game's map and
               could open a pie on ground the player never pointed at. The two
               focuses have to agree. */
            if (s_renderer == nullptr ||
                SDL_GetMouseFocus() != SDL_GetRenderWindow(s_renderer)) {
                return false;
            }
            code = (int)ev->key.scancode;
            SDL_GetMouseState(&mx, &my);
            /* Window coordinates, unlike the events, which the caller has
               already converted. */
            windowToScreen(mx, my, &px, &py);
        } else {
            return false;
        }
        if (code == 0) return false;

        windowGetKeys(&keys);
        mods   = currentMods();
        direct = pingBindingDirectKind(keys.kiPingDirect,
                                       PING_BIND_DIRECT_SLOTS, code, mods);
        /* The direct slots are asked first, so a chord the player has put on
           both a direct row and a menu row sends that kind rather than opening
           the pie: naming a kind is the more specific of the two. */
        if (direct < 0 &&
            pingBindingMatchAny(keys.kiPing, PING_BIND_SLOTS, code, mods) < 0) {
            return false;
        }
        if (!canPing(cs)) return false;
        /* The ping goes where the cursor was when the menu opened, so a press
           that started outside the map — or on a panel over it — has nothing
           to point at, and is left for whatever it was really aimed at. */
        if (!pingSurfaceWorldAt(cs, px, py, &s_worldX, &s_worldY)) return false;

        /* A direct chord is the whole action: the ping goes now, nothing is
           left armed and there is nothing to do on release. The press is still
           consumed, so it neither pans the overview nor builds. */
        if (direct >= 0) {
            clientSimNetSendPing(cs, (unsigned char)direct, s_worldX, s_worldY);
            return true;
        }

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

/* Pings inside the view are not drawn here: whichever map is up paints them
 * on the ground itself, between the terrain and the sprites, so tanks, shells
 * and builders stay on top — the classic frame in sdl3draw.c and the
 * overview's offscreen render in overview_view.cpp, both through
 * ping_marker.c. This overlay handles only what belongs above everything —
 * the pie menu, and the edge marker for a ping the view cannot show. */

/* Everything the edge markers need about whichever map is up: the rectangle a
 * bar may sit on, the origin a world position converts against, and the sizes
 * the chrome is drawn at. For the overview the two rectangles differ — the
 * markers are placed against the whole picture, but a bar is kept off the HUD
 * panels the host draws over its edges. */
typedef struct {
    PingSurfaceKind kind;
    float x, y, w, h;    /* where a bar may go, and what counts as on screen */
    float ox, oy;        /* the map's own origin, for world -> screen */
    float tileW, tileH;  /* one map square on screen */
    float scale;         /* what the chrome sizes are multiplied by */
} PingDrawView;

static bool pingDrawView(PingDrawView *out) {
    if (s_surface.kind == PING_SURFACE_CLASSIC) {
        PingViewRect vr;
        if (!viewRect(&vr)) return false;
        out->kind  = PING_SURFACE_CLASSIC;
        out->x     = out->ox = vr.x;
        out->y     = out->oy = vr.y;
        out->w     = vr.w;
        out->h     = vr.h;
        out->tileW = vr.tileW;
        out->tileH = vr.tileH;
        out->scale = vr.scale;
        return true;
    }
    if (s_surface.kind == PING_SURFACE_OVERVIEW) {
        PingRect r;
        float scale = sdl3ImguiGetUiScale();
        if (scale <= 0.0f) scale = 1.0f;
        /* Shrunk by what the host has drawn over the map, so a bar never ends
           up behind the status column, the build strip or the newswire. */
        pingRectInset(s_surface.x, s_surface.y, s_surface.w, s_surface.h,
                      s_surface.cam.insetL, s_surface.cam.insetT,
                      s_surface.cam.insetR, s_surface.cam.insetB, &r);
        out->kind  = PING_SURFACE_OVERVIEW;
        out->x     = r.x;
        out->y     = r.y;
        out->w     = r.w;
        out->h     = r.h;
        out->ox    = s_surface.x;
        out->oy    = s_surface.y;
        out->tileW = out->tileH =
            (float)OVERVIEW_TILE_PX * overviewCameraZoomScale(&s_surface.cam);
        out->scale = scale;
        return true;
    }
    return false;
}

/* A WORLD position in screen pixels, through whichever map is up. */
static void pingWorldToScreen(struct ClientSim *cs, const PingDrawView *v,
                              uint16_t worldX, uint16_t worldY,
                              float *outX, float *outY) {
    if (v->kind == PING_SURFACE_OVERVIEW) {
        float sx = 0.0f, sy = 0.0f;
        /* The camera takes fractional map squares, so the sub-square part of
           a WORLD coordinate survives the trip. */
        overviewCameraWorldToScreen(&s_surface.cam, s_surface.viewW,
                                    s_surface.viewH,
                                    (float)worldX / 256.0f,
                                    (float)worldY / 256.0f, &sx, &sy);
        *outX = v->ox + sx;
        *outY = v->oy + sy;
        return;
    }
    {
        PingViewRect vr;
        vr.x = v->ox; vr.y = v->oy; vr.w = v->w; vr.h = v->h;
        vr.tileW = v->tileW; vr.tileH = v->tileH; vr.scale = v->scale;
        classicWorldToScreen(cs, &vr, worldX, worldY, outX, outY);
    }
}

/* ------------------------------------------------------------------
 * The controller's half of the menu
 * ------------------------------------------------------------------
 *
 * A pad has no pointer, so none of the pointer half applies: the ping goes to
 * the square the build cursor is on (the one the player already aims builds
 * with), the pie is drawn over that square, and the sector is picked by
 * pointing the right stick or the d-pad rather than by moving a cursor.
 * Polled once a frame from pingOverlayDraw rather than driven by events,
 * because that is how the rest of the pad is read.
 */

/* The map square a pad ping lands on: the build cursor's target, or the
 * middle of whatever map is on screen when nothing has been targeted yet. */
static bool pingPadTargetSquare(struct ClientSim *cs, int *outX, int *outY) {
    BYTE bx = 0, by = 0;
    if (buildCursorGetTargetTile(&bx, &by)) {
        *outX = (int)bx;
        *outY = (int)by;
        return true;
    }
    if (s_surface.kind == PING_SURFACE_OVERVIEW) {
        *outX = (int)s_surface.cam.cx;
        *outY = (int)s_surface.cam.cy;
        return true;
    }
    if (s_surface.kind == PING_SURFACE_CLASSIC && cs != nullptr) {
        *outX = (int)clientSimGetXOffset(cs) + 1 + MAIN_SCREEN_SIZE_X / 2;
        *outY = (int)clientSimGetYOffset(cs) + 1 + MAIN_SCREEN_SIZE_Y / 2;
        return true;
    }
    return false;
}

/* Send one kind at the pad's target square. Nothing happens when there is no
 * map to point at, which is the same answer a press off the map gets. */
static void pingPadSendDirect(struct ClientSim *cs, unsigned char kind) {
    int mx = 0, my = 0;
    if (!canPing(cs)) return;
    if (!pingPadTargetSquare(cs, &mx, &my)) return;
    if (mx < 0 || my < 0 || mx > 255 || my > 255) return;
    clientSimNetSendPing(cs, kind, (uint16_t)(mx * 256 + 128),
                         (uint16_t)(my * 256 + 128));
}

/* Where the pad's pie is drawn: over its own square, pulled far enough inside
 * the view that the whole ring and its label are on screen. */
static void pingPadAnchor(struct ClientSim *cs, const PingDrawView *v,
                          int mapX, int mapY, float *outX, float *outY) {
    float scale = sdl3ImguiGetUiScale();
    float margin, ax = 0.0f, ay = 0.0f;
    if (scale <= 0.0f) scale = 1.0f;
    margin = PING_PIE_RADIUS_PX * scale + 8.0f * scale;

    pingWorldToScreen(cs, v, (uint16_t)(mapX * 256 + 128),
                      (uint16_t)(mapY * 256 + 128), &ax, &ay);
    if (v->w > margin * 2.0f) {
        if (ax < v->x + margin)        ax = v->x + margin;
        if (ax > v->x + v->w - margin) ax = v->x + v->w - margin;
    } else {
        ax = v->x + v->w * 0.5f;
    }
    if (v->h > margin * 2.0f) {
        if (ay < v->y + margin)        ay = v->y + margin;
        if (ay > v->y + v->h - margin) ay = v->y + v->h - margin;
    } else {
        ay = v->y + v->h * 0.5f;
    }
    *outX = ax;
    *outY = ay;
}

/* One frame of the pad's menu: the direct bindings, then opening, aiming and
 * releasing. `haveView` is false on a frame with no map, which closes an open
 * pad menu rather than leaving it aimed at nothing. */
static void pingPadTick(struct ClientSim *cs, const PingDrawView *v,
                        bool haveView) {
    int kind = -1;

    /* A direct binding is the whole action, exactly as its keyboard chord is:
       it sends now and leaves nothing armed. Consumed even when it cannot be
       acted on, so a press made in the lobby does not fire on the next frame
       that can. */
    if (inputGamepadConsumePingDirect(&kind) && haveView &&
        kind >= 0 && kind < PING_KIND_COUNT) {
        pingPadSendDirect(cs, (unsigned char)kind);
    }

    if (s_padOpen) {
        float aimX = 0.0f, aimY = 0.0f;
        float scale = sdl3ImguiGetUiScale();
        if (scale <= 0.0f) scale = 1.0f;
        if (!haveView || !canPing(cs)) {
            pingOverlayCancel();
            return;
        }
        /* Letting the binding go sends whatever is selected, the way letting
           the mouse button go does. */
        if (!inputGamepadIsPingMenuHeld()) {
            sendSelected(cs);
            pingOverlayCancel();
            return;
        }
        /* The aim stands in for the cursor: pointing puts it out in that
           slice, pointing nowhere leaves it in the dead zone, which is the
           standard ping. Placed at three quarters of the radius so it is
           unambiguously inside the ring rather than on its rim. */
        if (inputGamepadGetPingAim(&aimX, &aimY)) {
            float r = PING_PIE_RADIUS_PX * scale * 0.75f;
            s_cursorX = s_anchorX + aimX * r;
            s_cursorY = s_anchorY + aimY * r;
        } else {
            s_cursorX = s_anchorX;
            s_cursorY = s_anchorY;
        }
        return;
    }

    /* Opening. Not while the pointer already has a menu open — one pie at a
       time, and the pointer got there first. */
    if (s_open || !haveView) return;
    if (!inputGamepadIsPingMenuHeld()) return;
    if (!canPing(cs)) return;
    {
        int mx = 0, my = 0;
        if (!pingPadTargetSquare(cs, &mx, &my)) return;
        if (mx < 0 || my < 0 || mx > 255 || my > 255) return;
        s_worldX = (uint16_t)(mx * 256 + 128);
        s_worldY = (uint16_t)(my * 256 + 128);
        pingPadAnchor(cs, v, mx, my, &s_anchorX, &s_anchorY);
        s_cursorX     = s_anchorX;
        s_cursorY     = s_anchorY;
        s_triggerCode = 0;
        s_open        = true;
        s_padOpen     = true;
    }
}

void pingOverlayDraw(struct ClientSim *cs) {
    ClientPing    pings[MAX_CLIENT_PINGS];
    PingDrawView  vr;
    ImDrawList   *dl;
    uint32_t      now;
    int           n, i;
    float         tankX, tankY;
    float         tankMapX = 0.0f, tankMapY = 0.0f;
    bool          haveTank;
    bool          haveView;

    memset(&vr, 0, sizeof(vr));
    haveView = pingDrawView(&vr);

    /* The pad's menu is polled, not evented, so it is stepped here — before
       the pointer's stale-release check below, which does not apply to it. */
    pingPadTick(cs, &vr, haveView);

    /* A release that never arrived as an event. Something above the game can
       swallow the button-up — a modal opening under the player's finger, a
       focus hand-off — and the menu would then sit open over the view with no
       way to close it. If the trigger is no longer physically down, take that
       as the release it was: the player let go, and letting go is what sends
       the ping they had selected. The pad's menu has no trigger code to look
       up: pingPadTick above is what closes that one. */
    if (s_open && !s_padOpen && !pingTriggerHeld()) {
        sendSelected(cs);
        pingOverlayCancel();
    }

    if (s_open) drawMenu();

    if (cs == nullptr || !haveView) return;

    now = SDL_GetTicks();
    n = clientSimGetPings(cs, now, pings, MAX_CLIENT_PINGS);
    if (n <= 0) return;

    dl = ImGui::GetForegroundDrawList();

    /* The edge marker points from the viewer's tank. A dead or not-yet-placed
       tank has no position, in which case the centre of the view — which is
       where the camera is anyway — stands in for it. */
    haveTank = clientSimGetMyTankMapPosF(cs, &tankMapX, &tankMapY);
    if (haveTank) {
        pingWorldToScreen(cs, &vr,
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
        pingWorldToScreen(cs, &vr,
                          (uint16_t)((pings[i].worldX & 0xFF00u) | 0x80u),
                          (uint16_t)((pings[i].worldY & 0xFF00u) | 0x80u),
                          &px, &py);
        if (px >= vr.x && px <= vr.x + vr.w && py >= vr.y && py <= vr.y + vr.h) {
            continue;   /* on the ground, drawn by the game frame */
        }
        {
            PingEdgeMarker m;
            float thick  = PING_EDGE_THICKNESS_PX * vr.scale;
            float gap    = PING_EDGE_GAP_PX * vr.scale;
            float distTiles, sizeF, length, iconPx, ix, iy;

            /* How far the ping is, in map squares. Measured on screen and
               divided by what a square is drawn at, so the same arithmetic
               serves the classic view and the overview at any zoom — and so
               a viewer with no tank of its own, whose "tank" is the centre of
               the view, still gets a sensible number. */
            {
                float ddx = px - tankX, ddy = py - tankY;
                distTiles = (vr.tileW > 0.0f)
                          ? sqrtf(ddx * ddx + ddy * ddy) / vr.tileW : 0.0f;
            }
            sizeF  = pingEdgeSizeFactor(distTiles, PING_EDGE_NEAR_TILES,
                                        PING_EDGE_FAR_TILES);
            length = pingEdgeSizeFor(sizeF, PING_EDGE_LENGTH_MIN_PX,
                                     PING_EDGE_LENGTH_MAX_PX) * vr.scale;
            iconPx = pingEdgeSizeFor(sizeF, PING_EDGE_ICON_MIN_PX,
                                     PING_EDGE_ICON_MAX_PX) * vr.scale;

            if (!pingEdgeMarker(vr.x, vr.y, vr.w, vr.h, tankX, tankY, px, py,
                                length, &m)) {
                continue;
            }
            /* Pulled half a thickness inside the border so the whole bar is
               on the game view rather than half over the chrome. The midpoint
               moves with it: it is what the icon and the name are placed
               from. */
            if (m.side == PING_EDGE_LEFT)   { m.x0 += thick * 0.5f; m.x1 += thick * 0.5f; m.cx += thick * 0.5f; }
            if (m.side == PING_EDGE_RIGHT)  { m.x0 -= thick * 0.5f; m.x1 -= thick * 0.5f; m.cx -= thick * 0.5f; }
            if (m.side == PING_EDGE_TOP)    { m.y0 += thick * 0.5f; m.y1 += thick * 0.5f; m.cy += thick * 0.5f; }
            if (m.side == PING_EDGE_BOTTOM) { m.y0 -= thick * 0.5f; m.y1 -= thick * 0.5f; m.cy -= thick * 0.5f; }
            dl->AddLine(ImVec2(m.x0, m.y0), ImVec2(m.x1, m.y1),
                        IM_COL32(0, 0, 0, (int)(140 * alpha)), thick + 2.0f);
            dl->AddLine(ImVec2(m.x0, m.y0), ImVec2(m.x1, m.y1),
                        kindColour(pings[i].kind, alpha), thick);

            /* The kind's icon just inside the bar, so the colour alone does
               not have to carry which ping it is. It shrinks with the bar,
               down to the floor in ping_kinds.h below which a glyph stops
               reading as anything. */
            pingEdgeIconCentre(&m, thick, iconPx, gap, &ix, &iy);
            dl->AddCircleFilled(ImVec2(ix, iy), iconPx * 0.6f,
                                IM_COL32(0, 0, 0, (int)(150 * alpha)), 24);
            drawIcon(dl, pings[i].kind, ix, iy, iconPx, alpha);

            /* Who sent it, beside the icon and inside the view — the whole
               point of the indicator is knowing who is on their way without
               having to find them on the map first. In the overlay's own font
               rather than the tank labels' TTF face, which this ImGui draw
               list has no way to render, but over the same black shadow and
               at the same PING_NAME_SCALE of the text around it and the same
               PING_NAME_GREY the marker's own name uses, so a ping name is
               one thing wherever it is drawn.

               Unlike the bar it does not shrink with the distance: a name too
               small to read says nothing at all. */
            {
                /* Shortened the same way the marker's name is, ending in the
                   same "…": this draw list uses the ImGui atlas, which
                   imguiBoloGlyphRanges builds with U+2026 in it for exactly
                   this label. */
                char        shown[PING_NAME_DISPLAY_MAX];
                const char *who = pingDisplayName(pings[i].senderName,
                                                  PING_NAME_ELLIPSIS,
                                                  shown, sizeof(shown));
                if (who[0] != '\0') {
                    /* Measured at the size it is drawn at, not at the font's
                       own: the anchor box centres the text on the icon and
                       clamps it into the view off these two numbers, so a
                       width from the wrong size would put it off centre and
                       let it hang out of the rectangle at a corner. */
                    ImFont *fnt  = ImGui::GetFont();
                    float   fpx  = ImGui::GetFontSize() * PING_NAME_SCALE;
                    ImVec2  sz   = fnt->CalcTextSizeA(fpx, FLT_MAX, 0.0f, who);
                    PingEdgeNameBox box;
                    int   ia = (int)(alpha * 255.0f + 0.5f);
                    float sh = vr.scale;
                    if (sh < 1.0f) sh = 1.0f;
                    if (pingEdgeNameAnchor(m.side, ix, iy, iconPx, gap,
                                           sz.x, sz.y, vr.x, vr.y, vr.w, vr.h,
                                           &box)) {
                        dl->AddText(fnt, fpx, ImVec2(box.x + sh, box.y + sh),
                                    IM_COL32(0, 0, 0, ia), who);
                        dl->AddText(fnt, fpx, ImVec2(box.x, box.y),
                                    IM_COL32(PING_NAME_GREY, PING_NAME_GREY,
                                             PING_NAME_GREY, ia), who);
                    }
                }
            }
        }
    }
}
