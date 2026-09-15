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
 * Name:          overview_view.cpp
 * Purpose:       Implementation of the map overview's
 *                drawing and input — see overview_view.h.
 *                Terrain and mines from a snapshot of the
 *                client's OverviewMap — filled by the host
 *                under the client mutex, drawn here after it —
 *                the fog that dims everything outside a live
 *                region over the top of them, then the tanks,
 *                men and shells standing on the squares it
 *                says are visible; the camera maths it sits on
 *                lives in overview_camera.cpp and the fog mask
 *                in overview_fog.cpp.
 *
 *                Death is answered here too: the fog closing
 *                over the wreck is the sim's doing, and this
 *                file draws the static that follows it — the
 *                map goes dark, then the snow comes up and
 *                holds until the tank is back. The respawn
 *                takes that away and hands the map back from
 *                the square the tank came back on: everything
 *                else stays dark for a moment while a ring
 *                closes onto the tank.
 *********************************************************/

#include <SDL3/SDL.h>

#include "imgui.h"

#include "overview_view.h"
#include "overview_fog.h"   /* overviewFogBuildMask and the fog's constants */
#include "fog_roads_draw.h" /* the road outlines the Darker + roads look adds */
#include "key_claims.h"     /* keyIsClaimedByGame */
#include "build_cursor.h"   /* buildCursorSetTile */

extern "C" {
#include "global.h"
#include "client_sim.h"     /* the overviewSnapshot* readers the render draws
                               from; clientSimGetMyTankMapPosF,
                               clientSimIsMyTankAlive, clientSimManMoveToMap
                               and clientSimGetCurrentBuildSelect for input */
#include "../clientmutex.h" /* the build dispatch runs on the sim's data */
#include "cursor.h"         /* cursorSetCursor — the game's crosshair pointer */
#include "input.h"          /* inputBumpGunsight — the wheel's other job */
#include "overview_types.h"
#include "screentank.h"
#include "screenlgm.h"
#include "screenbullet.h"
#include "../tiles.h"       /* MINE_X / MINE_Y, TILE_SIZE_X / TILE_SIZE_Y */
#include "sprite_positions.h"
#include "mapview.h"         /* MapViewCtx */
#include "mapview_overlay.h" /* mapViewDrawOverlay — the whole entity layer */
#include "gfx_settings.h"    /* gfxGetFogStyle — the player's fog of war look */
#include "../ping_kinds.h"   /* pingDisplayAlpha */
#include "ping_marker.h"     /* pingMarkerDraw — the on-map ping pass */
#include "ping_overlay.h"    /* pingOverlayIsMenuOpen — the wheel's gate */
#include "ring_band.h"       /* the respawn ring's band, sides and curve */
}
#include "sdl3draw_status.h" /* sdl3DrawGetMessageFont, sdl3DrawGetLabelFont,
                                sdl3DrawGetTinyFont — the main window's faces */
#include "sdl3draw.h"        /* sdl3DrawGetZoomFactor — the size they are opened at */
#include "tank_label.h"      /* the cache the shared drawer fills for this view */

/* The Edit-menu Smooth Scrolling preference (winbolo.c). On, follow glides
 * with the tank's sub-square position; off, it steps whole squares the way
 * the classic view's scroll does. */
extern "C" bool smoothScrollingEnabled;

/* The View-menu Pillbox Labels and Base Labels switches (winbolo.c), read
 * here so the in-window and pop-out hosts draw the numbers by one rule. */
extern "C" bool showPillLabels;
extern "C" bool showBaseLabels;

/* View pixels an arrow key moves the centre per frame. In pixels rather than
 * squares so the map slides at the same apparent speed at every zoom. */
#define OVERVIEW_ARROW_STEP_PX 12.0f

/* Side of the crosshair sprite in game pixels. One more than a tile, so it
 * straddles the aim point rather than sitting in a corner of it. */
#define OVERVIEW_CROSSHAIR_PX 17

/* Zoom at or above which tank names and the pill and base numbers are drawn.
 * Below it the labels are wider than the tanks are apart and the picture
 * turns into text. */
#define OVERVIEW_LABEL_MIN_ZOOM 1.0f

/* The death blackout, drawn over the whole view from the tick the sim says a
 * death has stopped being watchable (clientSimIsMyTankDeathBlackout) through
 * to the respawn. The player watches their own explosion up to that point and
 * black is the last thing they see before the tank is back; it is what keeps
 * a dead player from sitting over the square that killed them and watching the
 * killer reposition, which is the same job the classic view's static does.
 *
 * A screen of snow the size of this one is a lot of noise for that, so this
 * view fades instead: the picture goes down to black over
 * OVERVIEW_DEATH_BLACK_FADE_MS and holds there, and the respawn cuts back to
 * the map. The memory underneath keeps stamping the whole time — what is
 * being taken away is the picture, not the block. */
#define OVERVIEW_DEATH_BLACK_FADE_MS 700    /* to reach full black */

/* The respawn ring, and the other half of that answer. The blackout stops
 * drawing on the frame the tank is back — the map cuts in, somewhere the
 * player may not have been looking when they died — so a circle closes on the
 * new square in the half second after it and says where the tank is.
 *
 * The radii are in map squares rather than view pixels, so the ring opens
 * seven squares across at every rung of the zoom ladder. It closes to a little
 * wider than the tank's own square, then opens slightly and shuts again as it
 * fades out, which is what keeps it from ending on a cut. */
#define OVERVIEW_RESPAWN_RING_START_SQ 3.5f   /* radius: seven squares across */
#define OVERVIEW_RESPAWN_RING_END_SQ   0.6f   /* just outside the tank sprite */
#define OVERVIEW_RESPAWN_RING_PULSE_SQ 0.85f  /* how far the pulse reopens */
#define OVERVIEW_RESPAWN_RING_MS       550    /* the close */
#define OVERVIEW_RESPAWN_RING_PULSE_MS 200    /* the pulse, and the fade with it */
#define OVERVIEW_RESPAWN_RING_WEIGHT   6.0f   /* px of stroke, across the band */

/* How many sides the drawn circle has is ring_band.h's business now
 * (RING_BAND_SIDE_PX / MIN_SEG / MAX_SEG), along with the band itself and the
 * two-stage curve above: the smart ping's world marker closes a ring of its
 * own in the same way, so the drawing is shared and only the numbers here are
 * this view's. */

/* The spotlight the ring arrives in: the map dark everywhere but a disc round
 * the tank, lifting over the moment after. The lit disc is wider than the ring
 * opens, so the ring is inside the light rather than crossing its edge, and it
 * opens a little further as the dark goes. Radii in map squares, like the
 * ring's. */
#define OVERVIEW_RESPAWN_DIM_MS        600  /* dark at the cut, gone by here */
#define OVERVIEW_RESPAWN_DIM_ALPHA     170  /* how dark, at the cut */
#define OVERVIEW_RESPAWN_DIM_HOLE_SQ   4.5f /* lit radius at the cut */
#define OVERVIEW_RESPAWN_DIM_OPEN_SQ   7.0f /* and where it has opened to */
#define OVERVIEW_RESPAWN_DIM_FEATHER_SQ 2.5f /* squares of soft edge */

/* The frame drawn round the picture while an item view is on. The weight is
 * taken from the view's height so it holds up on a small screen as well as a
 * desktop one, and capped so inset + weight never passes HUD_MARGIN (8): that
 * is what keeps the runs clear of the status column on the right and the build
 * strip on the left rather than under them. */
#define OVERVIEW_ITEM_BORDER_INSET 2      /* px from the picture's edge */
#define OVERVIEW_ITEM_BORDER_DIV   240    /* view height per px of weight */
#define OVERVIEW_ITEM_BORDER_MIN   2
#define OVERVIEW_ITEM_BORDER_MAX   6      /* inset + weight stays inside HUD_MARGIN (8) */

struct OverviewView {
    OverviewCamera cam;

    /* The offscreen the host blits. Textures belong to the renderer that
     * made them, so the renderer is tracked alongside the size and all
     * three are what a reallocation tests. */
    SDL_Texture   *target;
    SDL_Renderer  *targetRenderer;
    int            targetW;
    int            targetH;

    /* The fog overlay: one texel to a map square, stretched over the whole map
     * and sampled nearest, so a boundary lands on the square it belongs to at
     * every zoom. Bound to a renderer the same way the offscreen above is. The
     * mask is rebuilt only when the live regions move — the tank crossing a
     * square, or a pill's view coming and going — so fogLive/fogLiveCount hold
     * the set it was last built from and fogValid says whether they mean
     * anything yet. */
    SDL_Texture   *fog;
    SDL_Renderer  *fogRenderer;
    OverviewRect   fogLive[OVERVIEW_MAX_REGIONS];
    int            fogLiveCount;
    bool           fogValid;
    BYTE           fogMask[OVERVIEW_FOG_MASK_BYTES];

    /* Ground inside a region the player cannot see into — behind a building
     * with line of sight on: full fog for each square the map has marked
     * hidden, and nothing for the rest. The set of hidden squares moves as the
     * tank moves without a single rect moving, so the map generation the mask
     * was last built at is held with it, plus whether the map hid anything at
     * all, which catches the tick the mode is dropped and the rects sit
     * still. */
    BYTE           fogDark[OVERVIEW_FOG_SQUARE_BYTES];
    unsigned       fogGeneration;
    bool           fogHiddenActive;

    /* The OS pointer is switched to the game's crosshair while it is over the
     * map, so the view has to remember that it did the switching — nothing
     * else will put the system cursor back. dragWasActive carries the drag
     * one frame further, for the reason in overviewViewHandleInput. */
    bool           crosshairOn;
    bool           dragWasActive;

    /* Last frame's state of the scancodes the view reads straight off the
     * keyboard, so a binding can be answered on its edge — one action per
     * press however long the key is held. Indexed by scancode; only the
     * entries overviewBindingPressed is asked about mean anything. */
    bool           bindingWasDown[SDL_SCANCODE_COUNT];

    /* True while the pointer sits over a map square. The hover keeps the
     * shared build cursor on that square, and the main view draws its mouse
     * square solid — so this view does too, keeping the faint rendering for
     * a target the gamepad flow left behind. */
    bool           mouseOnMap;

    /* When the death blackout came up, and 0 when there is none — the edge the
     * fade is measured from, so one death is one fade however many frames the
     * view draws in it. */
    Uint64         blackoutTick;

    /* The local tank's state as the last frame left it, and when the respawn
     * ring started — 0 when none is running.
     *
     * aliveKnown is what keeps the first frame from reading as a respawn: the
     * view is built the first time full screen draws, which can be in the
     * middle of a game, and a tank that has simply always been alive would
     * otherwise come up as one that had just come back. The first frame
     * records what it finds and draws nothing. */
    bool           aliveKnown;
    bool           wasAlive;
    Uint64         respawnTick;

    /* The item view as it stood last frame, and what the camera did about it.
     * Entering one saves the follow flag and turns following on, aimed at the
     * watched item; stepping to another item starts a fresh scroll; leaving
     * puts the flag back and brings the tank on screen. Which of those it is
     * comes from comparing this frame's kind and target against these, and a
     * fresh view has never been in one.
     *
     * scrollTick is the SDL_GetTicks() the last scroll advance was measured
     * from, and 0 before there has been one. */
    bool           wasInItemView;
    uint8_t        wasViewKind;
    BYTE           wasViewTarget;
    bool           followBeforeItemView;
    Uint64         scrollTick;

    /* This view's tank-label cache — the shared drawer in tank_label.c
     * builds its textures on whichever renderer hosts the view (the classic
     * pass's cache is the main window's and cannot be shared). It flushes
     * itself when the font (reopened on zoom change) or the renderer
     * changes. */
    TankLabelCache labelCache;
    /* The names under this view's smart-ping markers. Separate from the tank
     * names above because both caches are keyed on the player slot and hold
     * different text for it — the note in sdl3draw_status.c has the whole
     * story. */
    TankLabelCache pingNameCache;
    /* The pill and base numbers, on this view's renderer for the same reason
     * the tank names are: the pop-out has its own. */
    ItemLabelCache itemLabelCache;
};

/* (Re)create the offscreen when the host asks for a size — or a renderer —
 * the current one does not match. Returns false when there is no target to
 * draw into. */
static bool overviewViewEnsureTarget(OverviewView *v, SDL_Renderer *r,
                                     int w, int h) {
    if (v->target && v->targetRenderer == r &&
        v->targetW == w && v->targetH == h) {
        return true;
    }

    if (v->target) {
        SDL_DestroyTexture(v->target);
        v->target = NULL;
    }
    v->targetRenderer = NULL;
    v->targetW = 0;
    v->targetH = 0;

    v->target = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA8888,
                                  SDL_TEXTUREACCESS_TARGET, w, h);
    if (!v->target) return false;

    SDL_SetTextureScaleMode(v->target, SDL_SCALEMODE_NEAREST);
    v->targetRenderer = r;
    v->targetW = w;
    v->targetH = h;
    return true;
}

/* Every square the view covers, all at full brightness — what the player can
 * see this instant and what they are only remembering alike. The fog pass
 * below takes the second kind back down; keeping the two apart is what lets
 * the fog go over the lot in one blit rather than a colour mod per tile. */
static void overviewViewDrawTerrain(SDL_Renderer *r, SDL_Texture *tiles, int ss,
                                    const OverviewCamera *cam,
                                    int viewW, int viewH,
                                    const OverviewMap *om,
                                    int left, int top, int right, int bottom) {
    /* The in-window overview draws from the main window's tile sheet, which
     * the classic view mods for its own purposes. */
    SDL_SetTextureColorMod(tiles, 255, 255, 255);

    float tilePx = (float)OVERVIEW_TILE_PX * overviewCameraZoomScale(cam);
    SDL_FRect mineSrc = mapViewAtlasSrc(MINE_X, MINE_Y,
                                        TILE_SIZE_X, TILE_SIZE_Y, ss);

    /* x outer, y inner: the memory is [x][y], so the inner walk is
     * contiguous. */
    for (int mx = left; mx <= right; mx++) {
        for (int my = top; my <= bottom; my++) {
            BYTE tile = om->tile[mx][my];
            if (tile == OVERVIEW_UNSEEN) continue;  /* the black clear shows */

            BYTE flags = om->flags[mx][my];
            float sx = 0.0f, sy = 0.0f;
            overviewCameraWorldToScreen(cam, viewW, viewH,
                                        (float)mx, (float)my, &sx, &sy);
            /* Whole pixels. The camera is continuous, so a tile boundary can
             * land on a half-pixel, and neighbouring tiles then either leave
             * a gap that shows the black clear colour or sample a texel from
             * the next atlas cell. Rounding here is exact rather than
             * approximate: tilePx is OVERVIEW_TILE_PX * zoomScale, and every
             * rung of the zoom ladder makes that a whole number, so
             * round(sx + tilePx) == round(sx) + tilePx. Tiles keep their
             * exact size and abut. */
            sx = SDL_roundf(sx);
            sy = SDL_roundf(sy);
            SDL_FRect dest = { sx, sy, tilePx, tilePx };
            SDL_FRect src  = mapViewAtlasSrc(mapViewPosX[tile],
                                             mapViewPosY[tile],
                                             TILE_SIZE_X, TILE_SIZE_Y, ss);
            SDL_RenderTexture(r, tiles, &src, &dest);

            if ((flags & OVERVIEW_F_MINE) != 0) {
                SDL_RenderTexture(r, tiles, &mineSrc, &dest);
            }
        }
    }
}

/* (Re)create the fog texture when the renderer changes. White, so the fog's
 * colour is the colour mod and nothing else and the mask is carried by the
 * alpha alone. Nothing else ever draws this texture, so — unlike the host's
 * tile sheet, which ImGui also submits — the sampler set here survives from
 * frame to frame. */
static bool overviewViewEnsureFog(OverviewView *v, SDL_Renderer *r) {
    if (v->fog && v->fogRenderer == r) return true;

    if (v->fog) {
        SDL_DestroyTexture(v->fog);
        v->fog = NULL;
    }
    v->fogRenderer = NULL;
    v->fogValid = false;

    v->fog = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA8888,
                               SDL_TEXTUREACCESS_STREAMING,
                               OVERVIEW_FOG_MASK_SIDE, OVERVIEW_FOG_MASK_SIDE);
    if (!v->fog) return false;

    SDL_SetTextureBlendMode(v->fog, SDL_BLENDMODE_BLEND);
    /* One texel per square, blown up to a whole tile. Linear filtering would
     * shade between neighbouring texels and blur every boundary the mask draws,
     * so a square would come out part lit whatever byte it was given; nearest
     * keeps the edge where the mask puts it. The view target above is set the
     * same way. */
    SDL_SetTextureScaleMode(v->fog, SDL_SCALEMODE_NEAREST);
    /* The fog's colour is the colour mod — src is white, so that alone picks
     * it — and the player can change it while the game runs, so it is set per
     * frame in overviewViewDrawFog rather than once here. */
    v->fogRenderer = r;
    return true;
}

/* Rebuild the mask from the regions and push it into the texture. RGBA8888 is
 * one Uint32 per texel with red in the top byte, so a white texel carrying the
 * mask as its alpha is 0xFFFFFF00 | mask.
 *
 * A square the map has marked hidden is handed over at full fog: it sits inside
 * the block, so the regions would otherwise leave it clear. The map's arrays
 * are [x][y] and the mask is a texture row at a time, so the scratch is filled
 * transposed, and only when the map says it hid something this update. */
static void overviewViewUploadFog(OverviewView *v, const OverviewMap *om) {
    void       *pixels = NULL;
    int         pitch  = 0;
    const BYTE *dark   = NULL;

    if (om->hiddenActive) {
        for (int y = 0; y < MAP_ARRAY_SIZE; y++) {
            BYTE *row = v->fogDark + (size_t)y * MAP_ARRAY_SIZE;
            for (int x = 0; x < MAP_ARRAY_SIZE; x++) {
                row[x] = (om->flags[x][y] & OVERVIEW_F_HIDDEN) != 0
                             ? (BYTE)OVERVIEW_FOG_ALPHA
                             : (BYTE)0;
            }
        }
        dark = v->fogDark;
    }

    overviewFogBuildMask(om->live, om->liveCount, dark, v->fogMask);
    if (!SDL_LockTexture(v->fog, NULL, &pixels, &pitch)) return;

    for (int y = 0; y < OVERVIEW_FOG_MASK_SIDE; y++) {
        Uint32     *row = (Uint32 *)((Uint8 *)pixels + (size_t)y * (size_t)pitch);
        const BYTE *src = v->fogMask + (size_t)y * OVERVIEW_FOG_MASK_SIDE;
        for (int x = 0; x < OVERVIEW_FOG_MASK_SIDE; x++) {
            row[x] = 0xFFFFFF00u | (Uint32)src[x];
        }
    }
    SDL_UnlockTexture(v->fog);
}

/* The fog over the terrain the pass above just drew.
 *
 * The texture covers the whole map, one texel to a square, so it goes down as a
 * single blit of the map's own rect: a texel then spans exactly its square, and
 * sampled nearest it carries its own byte and none of its neighbours'. The
 * rect's origin is rounded the way the terrain's is, and its size is a whole
 * number of tiles, so the two stay registered at every zoom.
 *
 * The mask comes from the live regions rather than the per-square LIVE flag —
 * the sim writes the flag from those same rects, so they say the same thing,
 * and the rects are at most OVERVIEW_MAX_REGIONS structs to compare where the
 * flags are 64K of bytes. */
static void overviewViewDrawFog(OverviewView *v, SDL_Renderer *r,
                                const OverviewCamera *cam, int viewW, int viewH,
                                const OverviewMap *om,
                                int left, int top, int right, int bottom) {
    /* What the player has asked fog to look like. None draws nothing at all,
     * so the mask is not even rebuilt: the one that is already there stays
     * good, and picking a look that washes again uses it. */
    FogStyle      style = gfxGetFogStyle();
    unsigned char fogR = 0, fogG = 0, fogB = 0;

    if (fogLookColour(style, &fogR, &fogG, &fogB) == 0) return;
    if (!overviewViewEnsureFog(v, r)) return;
    SDL_SetTextureColorMod(v->fog, fogR, fogG, fogB);

    /* With line of sight on, the hidden squares move as the tank drives without
     * any rect moving, so the map's generation is what the mask is held against
     * — it counts up on any update that moved anything. Only then: generation
     * moves for a terrain change anywhere on the map, so reading it whatever
     * the mode would rebuild the mask far more often than the rects do.
     * hiddenActive changing is the mode being taken up or left, which has to
     * rebuild on its own account: the tick it is dropped the rects can sit
     * exactly where they were, and the mask would otherwise keep drawing fog
     * over squares the map no longer hides. */
    if (!v->fogValid || v->fogLiveCount != om->liveCount ||
        om->hiddenActive != v->fogHiddenActive ||
        (om->hiddenActive && om->generation != v->fogGeneration) ||
        SDL_memcmp(v->fogLive, om->live,
                   sizeof(OverviewRect) * (size_t)om->liveCount) != 0) {
        overviewViewUploadFog(v, om);
        SDL_memcpy(v->fogLive, om->live, sizeof(v->fogLive));
        v->fogLiveCount = om->liveCount;
        v->fogGeneration = om->generation;
        v->fogHiddenActive = om->hiddenActive;
        v->fogValid = true;
    }

    float tilePx = (float)OVERVIEW_TILE_PX * overviewCameraZoomScale(cam);
    float sx = 0.0f, sy = 0.0f;
    overviewCameraWorldToScreen(cam, viewW, viewH, 0.0f, 0.0f, &sx, &sy);

    SDL_FRect dst = { SDL_roundf(sx), SDL_roundf(sy),
                      tilePx * (float)MAP_ARRAY_SIZE,
                      tilePx * (float)MAP_ARRAY_SIZE };
    SDL_RenderTexture(r, v->fog, NULL, &dst);

    /* The fog line over road that the Darker + roads look draws back in, over
     * the blit that has just gone down. A second walk of the visible squares
     * rather than a pass folded into the terrain loop: the mask the bands are
     * gated on is built here, and a square is only reached at all if the mask
     * says it is fogged, which most of a zoomed-out map is not.
     *
     * The same mask says whether each neighbour is fogged, so the band lands
     * on exactly the line the blit above draws, on its fogged side.
     *
     * Dropped outright once a square is too small to hold the fade, which is
     * where the band would be a line over the whole square rather than an
     * edge on it — and where there are the most squares to walk. */
    if (fogLookDrawsRoadEdges(style) &&
        tilePx >= (float)(FOG_ROAD_BANDS * 2)) {
        const BYTE *mask = v->fogMask;
        FogRoadPainter painter;
        fogRoadPainterBegin(&painter, r);
        for (int mx = left; mx <= right; mx++) {
            for (int my = top; my <= bottom; my++) {
                size_t here = (size_t)my * OVERVIEW_FOG_MASK_SIDE + (size_t)mx;
                if (mask[here] == 0) continue;
                /* Off the map counts as fogged, so no band is drawn along the
                 * map border. The border is deep sea in every map the game
                 * ships, so this is a guard rather than a case that comes
                 * up. */
                int lf = (mx > 0) ? (mask[here - 1] != 0) : 1;
                int rf = (mx < MAP_ARRAY_SIZE - 1) ? (mask[here + 1] != 0) : 1;
                int uf = (my > 0)
                             ? (mask[here - OVERVIEW_FOG_MASK_SIDE] != 0) : 1;
                int df = (my < MAP_ARRAY_SIZE - 1)
                             ? (mask[here + OVERVIEW_FOG_MASK_SIDE] != 0) : 1;
                unsigned char edges =
                    fogEdges(om->tile[mx][my], 1, lf, rf, uf, df);
                if (edges == 0) continue;

                float ex = 0.0f, ey = 0.0f;
                overviewCameraWorldToScreen(cam, viewW, viewH, (float)mx,
                                            (float)my, &ex, &ey);
                /* Rounded the way the terrain pass rounds, so a band sits on
                 * the tile it belongs to rather than half a pixel off it. */
                fogRoadPainterSquare(&painter, edges, SDL_roundf(ex),
                                     SDL_roundf(ey), tilePx, tilePx);
            }
        }
        fogRoadPainterEnd(&painter);
    }
}

/* Black over the finished frame, for as long as the sim says the death is in
 * its last couple of seconds. It fades up over OVERVIEW_DEATH_BLACK_FADE_MS
 * and then holds, so the window darkens rather than cutting; the respawn ends
 * it outright, which is the cut back to the map the classic view makes too. */
static void overviewViewDrawDeathBlackout(OverviewView *v, SDL_Renderer *r,
                                          int viewW, int viewH,
                                          const OverviewSnapshot *snap) {
    bool showing = overviewSnapshotBlackout(snap);
    Uint64 now = SDL_GetTicks();

    if (!showing) {
        v->blackoutTick = 0;
        return;
    }
    if (v->blackoutTick == 0) {
        v->blackoutTick = now;
    }

    Uint64 elapsed = now - v->blackoutTick;
    Uint8  alpha   = 255;
    if (elapsed < OVERVIEW_DEATH_BLACK_FADE_MS) {
        alpha = (Uint8)(255.0f * (float)elapsed /
                        (float)OVERVIEW_DEATH_BLACK_FADE_MS);
    }

    SDL_FRect dst = { 0.0f, 0.0f, (float)viewW, (float)viewH };
    SDL_BlendMode was = SDL_BLENDMODE_NONE;
    SDL_GetRenderDrawBlendMode(r, &was);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, 0, 0, 0, alpha);
    SDL_RenderFillRect(r, &dst);
    SDL_SetRenderDrawBlendMode(r, was);
}

/* The band between two circles this view draws its ring and the soft edge of
 * its spotlight out of, and the sides it builds one from, are ringBandDraw and
 * ringBandSegments in ring_band.h. They used to be here; they moved out
 * unchanged when the smart-ping world marker started closing a ring of its
 * own. Note that neither touches the blend mode — the caller below sets
 * SDL_BLENDMODE_BLEND once around the whole effect. */

/* The spotlight: the map goes dark everywhere but a disc round the tank, and
 * the dark lifts over the moment after. What it is for is the frame the map
 * cuts back in — the tank is then the one lit thing on the picture, which is a
 * harder cue to miss than any mark drawn on top of a map that is all equally
 * bright.
 *
 * Three pieces: the disc itself, which is left alone; a band round it that
 * ramps from clear to the full dim, so the edge of the light is soft rather
 * than a stencil; and everything beyond that band at the full dim, which is
 * one more band taken out past the corner of the view. The disc opens as the
 * dark fades, so the light spreads from the tank outwards rather than the
 * picture just getting brighter.
 *
 * The panels are not dimmed: they are drawn by the host after this offscreen
 * is blitted, so what a player reads to find out how they died stays as it
 * was. */
static void overviewViewDrawRespawnDim(SDL_Renderer *r, float cx, float cy,
                                       int viewW, int viewH, float zoomScale,
                                       Uint64 elapsed) {
    if (elapsed >= OVERVIEW_RESPAWN_DIM_MS) return;

    float t = (float)elapsed / (float)OVERVIEW_RESPAWN_DIM_MS;
    float u = 1.0f - t;

    /* Squared, so the dark is deepest at the cut and most of it is gone by
     * halfway — the cue lands, then the map is handed back. */
    Uint8 alpha = (Uint8)((float)OVERVIEW_RESPAWN_DIM_ALPHA * u * u);
    if (alpha == 0) return;

    float tilePx  = (float)OVERVIEW_TILE_PX * zoomScale;
    float hole    = (OVERVIEW_RESPAWN_DIM_HOLE_SQ +
                     (OVERVIEW_RESPAWN_DIM_OPEN_SQ -
                      OVERVIEW_RESPAWN_DIM_HOLE_SQ) * t) * tilePx;
    float feather = OVERVIEW_RESPAWN_DIM_FEATHER_SQ * tilePx;

    /* Far enough out to cover the corner of the view from wherever the tank
     * is, with the slack a polygon needs: its flat sides fall inside the
     * radius they are built at. */
    float dx    = (cx > (float)viewW * 0.5f) ? cx : (float)viewW - cx;
    float dy    = (cy > (float)viewH * 0.5f) ? cy : (float)viewH - cy;
    float reach = SDL_sqrtf(dx * dx + dy * dy) * 1.5f + hole + feather;

    int segments = ringBandSegments(hole + feather);

    ringBandDraw(r, cx, cy, hole, hole + feather, segments,
                 0, 0, 0, 0, alpha);
    ringBandDraw(r, cx, cy, hole + feather, reach, segments,
                 0, 0, 0, alpha, alpha);
}

/* The dead-to-alive edge, and what the camera does about it.
 *
 * The same edge the blackout ends on — both read clientSimIsMyTankAlive — so
 * the ring it starts is on the picture from the first frame there is a map to
 * draw it over. Dying again ends a ring still running: the next respawn is a
 * fresh one, not the rest of the old.
 *
 * Answered here rather than where the ring is drawn because the other half of
 * the answer is a camera move, and the camera is settled further down this
 * frame: a scroll started now is advanced by the same frame's tick.
 *
 * Following brings the tank back by itself, so that scroll is for the free
 * camera — a player who panned off and would otherwise come back with their
 * tank behind the newswire, under the status column or off the picture
 * altogether. It moves the least it can and counts the panels as covered, so a
 * tank that came back somewhere already clear leaves the camera exactly where
 * the player parked it. */
static void overviewViewTickRespawn(OverviewView *v, int viewW, int viewH,
                                    const OverviewSnapshot *snap) {
    bool alive = overviewSnapshotTankAlive(snap);

    /* The first frame the view sees only records what it found: see
     * aliveKnown. */
    if (!v->aliveKnown) {
        v->aliveKnown = true;
        v->wasAlive   = alive;
        return;
    }

    bool respawned = alive && !v->wasAlive;
    v->wasAlive = alive;
    if (!alive) {
        v->respawnTick = 0;
        return;
    }
    if (!respawned) return;

    v->respawnTick = SDL_GetTicks();

    float tankX = 0.0f, tankY = 0.0f;
    if (overviewSnapshotTankPos(snap, &tankX, &tankY)) {
        overviewCameraScrollToShow(&v->cam, viewW, viewH, tankX, tankY);
    }
}

/* What that edge starts: the map cuts back on the respawn frame with the tank
 * the one lit thing on it, and the light spreads while a ring closes onto the
 * tank over the half second that follows.
 *
 * The centre is read fresh every frame rather than pinned where the tank came
 * back, so a player who is already driving keeps the light and the ring on the
 * tank instead of leaving them over the ground they spawned on. */
static void overviewViewDrawRespawn(OverviewView *v, SDL_Renderer *r,
                                    int viewW, int viewH,
                                    const OverviewSnapshot *snap) {
    if (v->respawnTick == 0 || snap == NULL) return;

    /* The ring's two stages outlast the dim, so their end is the whole
     * effect's. */
    Uint64 elapsed = SDL_GetTicks() - v->respawnTick;
    if (elapsed >= OVERVIEW_RESPAWN_RING_MS + OVERVIEW_RESPAWN_RING_PULSE_MS) {
        v->respawnTick = 0;
        return;
    }

    /* The tank's own sub-square position, so the light and the ring sit on the
     * sprite and not on the corner of its square. Gone means there is nothing
     * to mark — a disconnect, or the tank going in the frame this started. */
    float tankX = 0.0f, tankY = 0.0f;
    if (!overviewSnapshotTankPos(snap, &tankX, &tankY)) return;

    float cx = 0.0f, cy = 0.0f;
    overviewCameraWorldToScreen(&v->cam, viewW, viewH, tankX, tankY, &cx, &cy);
    float zoomScale = overviewCameraZoomScale(&v->cam);

    SDL_BlendMode was = SDL_BLENDMODE_NONE;
    SDL_GetRenderDrawBlendMode(r, &was);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);

    /* The dark first, so the ring is drawn into the light it leaves rather
     * than through it. */
    overviewViewDrawRespawnDim(r, cx, cy, viewW, viewH, zoomScale, elapsed);

    /* The close and the pulse, from the shared curve (ring_band.h). The
     * radii stay here in map squares, so the ring opens seven squares across
     * at every rung of the zoom ladder whatever the ping marker's ring is
     * doing with the same arithmetic.
     *
     * The defaults are what a finished ring would leave, which the guard at
     * the top of this function already means cannot happen — kept so it stays
     * right if that guard changes. */
    const RingAnim ring = { OVERVIEW_RESPAWN_RING_START_SQ,
                            OVERVIEW_RESPAWN_RING_END_SQ,
                            OVERVIEW_RESPAWN_RING_PULSE_SQ,
                            OVERVIEW_RESPAWN_RING_MS,
                            OVERVIEW_RESPAWN_RING_PULSE_MS };
    float radiusSq = OVERVIEW_RESPAWN_RING_END_SQ;
    float fade     = 1.0f;
    ringAnimAt(&ring, elapsed, &radiusSq, &fade);
    Uint8 alpha = (Uint8)(255.0f * fade);

    float radiusPx = radiusSq * (float)OVERVIEW_TILE_PX * zoomScale;
    if (radiusPx >= 1.0f) {
        /* The yellow the item-view border is drawn in — the view's one
         * accent — at one alpha across the band, so it fades flat. */
        ringBandDraw(r, cx, cy,
                     radiusPx - OVERVIEW_RESPAWN_RING_WEIGHT * 0.5f,
                     radiusPx + OVERVIEW_RESPAWN_RING_WEIGHT * 0.5f,
                     ringBandSegments(radiusPx),
                     255, 205, 40, alpha, alpha);
    }
    SDL_SetRenderDrawBlendMode(r, was);
}

/* The frame that says an item view is on. In the classic view the item view
 * replaces the 15x15, so it is obvious; here the map goes on being drawn, and
 * with the key view rule the block round the tank closes while the view is up,
 * so without this the picture can go dark with nothing to say why.
 *
 * One yellow whatever the kind of view: the caption along the bottom names it,
 * so the border only has to say that one is on. Not the HUD greys, which read
 * as chrome. Rectangle fills only — no texture and no font — so it is valid on
 * either host's renderer. The alpha is 255, so whatever blend mode the entity
 * pass left behind gives the same result and there is nothing to save or put
 * back. */
static void overviewViewDrawItemViewBorder(SDL_Renderer *r, int viewW, int viewH) {
    int weight = (int)SDL_roundf((float)viewH / (float)OVERVIEW_ITEM_BORDER_DIV);
    if (weight < OVERVIEW_ITEM_BORDER_MIN) weight = OVERVIEW_ITEM_BORDER_MIN;
    if (weight > OVERVIEW_ITEM_BORDER_MAX) weight = OVERVIEW_ITEM_BORDER_MAX;

    float inset = (float)OVERVIEW_ITEM_BORDER_INSET;
    float x = inset;
    float y = inset;
    float bw = (float)viewW - 2.0f * inset;
    float bh = (float)viewH - 2.0f * inset;
    if (bw <= 0.0f || bh <= 0.0f) return;

    /* A view small enough for the opposite runs to meet is filled solid
     * instead, rather than asking SDL to draw sides of negative height. */
    float wt   = (float)weight;
    float half = (bw < bh ? bw : bh) * 0.5f;
    if (wt > half) wt = half;

    /* The top and bottom runs span the full width and the sides fit between
     * them, so the corners are covered and the rectangle closes. */
    SDL_FRect runs[4] = {
        { x,           y,           bw, wt },
        { x,           y + bh - wt, bw, wt },
        { x,           y + wt,      wt, bh - 2.0f * wt },
        { x + bw - wt, y + wt,      wt, bh - 2.0f * wt },
    };
    SDL_SetRenderDrawColor(r, 255, 205, 40, 255);
    SDL_RenderFillRects(r, runs, 4);
}

/* The pill and base numbers the shared pass draws. The snapshot lists every
 * pill and base at its square; one is kept only when the memory shows a pill
 * or base tile on that square and the square is live — the tile test the
 * classic view makes on its screen buffer, made here on the same copy the
 * sprites were filtered against. A carried pill, a destroyed one and a
 * remembered square all fail it, so none of them shows a number. The Pillbox
 * Labels and Base Labels switches apply as they do in the classic view. */
static int overviewViewPickItemLabels(const OverviewSnapshot *snap,
                                      OverviewItemLabel *out, int cap) {
    const OverviewMap *om = overviewSnapshotMap(snap);
    if (om == NULL) return 0;

    int total = overviewSnapshotItemLabelCount(snap);
    const OverviewItemLabel *all = overviewSnapshotItemLabels(snap);
    int n = 0;
    for (int i = 0; i < total && n < cap; i++) {
        const OverviewItemLabel *l = &all[i];
        bool wanted = l->isBase ? showBaseLabels : showPillLabels;
        if (!wanted) continue;
        if ((om->flags[l->mapX][l->mapY] & OVERVIEW_F_LIVE) == 0) continue;
        BYTE tile = om->tile[l->mapX][l->mapY];
        bool onTile = l->isBase ? mapViewTileIsBase(tile) : mapViewTileIsPill(tile);
        if (!onTile) continue;
        out[n++] = *l;
    }
    return n;
}

/* The sprite overlay: everything that moves, on the squares the player can
 * see this instant, with the player's own marks and the labels round it.
 * Runs on the offscreen the terrain passes just filled. The lists come out
 * of the snapshot already filtered to live squares, so nothing here decides
 * what is drawn — only where, through the same pass the classic view uses. */
static void overviewViewDrawEntities(OverviewView *v,
                                     SDL_Renderer *r, SDL_Texture *tiles, int ss,
                                     SDL_Texture *crosshair,
                                     const OverviewCamera *cam,
                                     int viewW, int viewH,
                                     const OverviewSnapshot *snap) {
    MapViewCtx        ctx;
    MapViewOverlay    ov;
    OverviewItemLabel items[MAX_PILLS + MAX_BASES];
    bool              selfDrawn = overviewSnapshotSelfDrawn(snap);
    /* mapview.c's passes only read the lists; their signatures predate
     * const, so the snapshot's are handed over through a cast. */
    screenTanks   *tks  = (screenTanks *)overviewSnapshotTanks(snap);
    screenLgm     *lgms = (screenLgm *)overviewSnapshotLgms(snap);
    screenBullets *sb   = (screenBullets *)overviewSnapshotBullets(snap);

    /* mapview.c positions a sprite at originX - tileW - edgeX + bbx * scale,
     * where bbx is the entity's game-pixel offset from the rect's origin. The
     * lists were built from a rect starting at 0,0, so bbx is the offset from
     * map square 0,0 and the whole transform reduces to placing that square:
     * hand it the camera's answer for 0,0 as the origin and the rung as the
     * scale, and every sprite lands where the camera would have put it. The
     * origin stays a float all the way down, so the camera's fractional
     * position is kept rather than rounded. tileW is also the size a tank
     * sprite is drawn at, so it is one square at this zoom. */
    float zoomScale = overviewCameraZoomScale(cam);
    float o0x = 0.0f, o0y = 0.0f;
    overviewCameraWorldToScreen(cam, viewW, viewH, 0.0f, 0.0f, &o0x, &o0y);

    float tileW   = (float)TILE_SIZE_X * zoomScale;
    float tileH   = (float)TILE_SIZE_Y * zoomScale;
    float originX = o0x + tileW;
    float originY = o0y + tileH;

    ctx.renderer   = r;
    ctx.tilesTex   = tiles;
    ctx.zoomFactor = 1;
    ctx.sheetScale = ss;
    ctx.scale      = zoomScale;
    /* The full screen map draws from the host's sheet and has no padded copy
       of its own, so its sprites still sample the packed layout. Its camera
       is continuous, which is the condition the leak needs, so this is the
       next one to hand an atlas — it wants the builder plumbed through
       overviewViewRenderOffscreen and through the copy sdl3imgui.cpp builds
       for the pop-out. */
    ctx.spritesTex = NULL;
    ctx.sprites    = NULL;

    SDL_memset(&ov, 0, sizeof(ov));

    /* Where a build will land, from the same mouse_square sprite the main view
     * draws. Solid while cursor mode is on or the pointer is over the map (the
     * main view draws its mouse square solid); a target that is only locked in
     * shows faint, which is the split the main view makes too. */
    BYTE bcX = 0, bcY = 0;
    bool cursorMode = buildCursorGetTile(&bcX, &bcY);
    ov.cursorShown = cursorMode || buildCursorGetTargetTile(&bcX, &bcY);
    ov.cursorFaint = !(cursorMode || v->mouseOnMap);
    ov.cursorMapX  = bcX;
    ov.cursorMapY  = bcY;

    /* The local player's own reticle, drawn only when the tank sprite was:
     * the reticle sits a gunsight's length from the tank, so putting it on
     * the picture while the tank is hidden would mark where the tank is just
     * as surely as the sprite did. It takes its answer from the same filter
     * the sprite went through, so the two cannot disagree. */
    ov.gunsightShown = crosshair != NULL && selfDrawn &&
                       overviewSnapshotGunsight(snap, &ov.gsMapX, &ov.gsMapY,
                                                &ov.gsPixelX, &ov.gsPixelY);
    ov.crosshairTex  = crosshair;
    ov.crosshairPx   = OVERVIEW_CROSSHAIR_PX;

    /* Tank names, through this view's own cache: the classic pass's textures
     * live on the main window's renderer and this view may be on the
     * pop-out's. The face is opened at 13 px times the main window's zoom;
     * the blit is scaled so the on-screen height is 13 px times the overview
     * zoom. Below OVERVIEW_LABEL_MIN_ZOOM no font is handed over, which is
     * how the pass is told to draw no names. */
    int mainZoom = sdl3DrawGetZoomFactor();
    if (mainZoom < 1) mainZoom = 1;
    ov.labelDisplayScale = zoomScale / (float)mainZoom;
    if (zoomScale >= OVERVIEW_LABEL_MIN_ZOOM) {
        ov.labelCache = &v->labelCache;
        ov.labelFont  = sdl3DrawGetMessageFont();
    }

    /* The pill and base numbers, from the same zoom up, in the main window's
     * faces scaled the same way as the names. */
    ov.itemLabelCount    = overviewViewPickItemLabels(snap, items,
                                                      (int)(MAX_PILLS + MAX_BASES));
    ov.itemLabels        = items;
    ov.pillFont          = sdl3DrawGetLabelFont();
    ov.baseFont          = sdl3DrawGetTinyFont();
    ov.itemLabelMinScale = OVERVIEW_LABEL_MIN_ZOOM;
    ov.itemLabelCache    = &v->itemLabelCache;

    ov.clipLeft   = 0.0f;
    ov.clipTop    = 0.0f;
    ov.clipRight  = (float)viewW;
    ov.clipBottom = (float)viewH;

    mapViewDrawOverlay(&ctx, &ov, tks, lgms, sb, originX, originY,
                       tileW, tileH, 0.0f, 0.0f);
}

extern "C" OverviewView *overviewViewCreate(void) {
    OverviewView *v = (OverviewView *)SDL_calloc(1, sizeof(*v));
    if (!v) return NULL;
    /* Idempotent, and the only thing that fills mapViewPosX/Y — calling it
     * here means the view does not depend on the main window having drawn
     * a frame first. */
    mapViewInit();
    overviewCameraInit(&v->cam);
    return v;
}

extern "C" void overviewViewDestroy(OverviewView *v) {
    if (!v) return;
    tankLabelCacheFlush(&v->labelCache);
    tankLabelCacheFlush(&v->pingNameCache);
    itemLabelCacheFlush(&v->itemLabelCache);
    if (v->fog) {
        SDL_DestroyTexture(v->fog);
        v->fog = NULL;
    }
    if (v->target) {
        SDL_DestroyTexture(v->target);
        v->target = NULL;
    }
    SDL_free(v);
}

extern "C" SDL_Texture *overviewViewGetTexture(const OverviewView *v) {
    return v ? v->target : NULL;
}

extern "C" void overviewViewGetSize(const OverviewView *v, int *outW, int *outH) {
    if (outW) *outW = v ? v->targetW : 0;
    if (outH) *outH = v ? v->targetH : 0;
}

extern "C" OverviewCamera *overviewViewCamera(OverviewView *v) {
    return v ? &v->cam : NULL;
}

extern "C" void overviewViewSetHudInsets(OverviewView *v, float left, float top,
                                         float right, float bottom) {
    if (!v) return;
    overviewCameraSetInsets(&v->cam, left, top, right, bottom);
}

extern "C" void overviewViewRenderOffscreen(OverviewView *v, SDL_Renderer *r,
                                            SDL_Texture *tiles, int sheetScale,
                                            SDL_Texture *crosshair,
                                            int w, int h,
                                            const OverviewSnapshot *snap,
                                            bool ownsWindow) {
    if (!v || !r || w <= 0 || h <= 0) return;
    if (!overviewViewEnsureTarget(v, r, w, h)) return;

    SDL_SetRenderTarget(r, v->target);
    SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
    SDL_RenderClear(r);

    /* Going into an item view, stepping to the next item and coming back out
     * of one. Only where this view has replaced the classic one: beside the
     * pop-out the 15x15 is already showing the item, and taking a deliberately
     * parked pop-out camera off where the player put it would be a worse
     * trade than leaving it on the tank.
     *
     * Ahead of the camera work below, so a scroll started here is advanced on
     * the same frame it starts and the per-frame follow stands down for it.
     * The other way round, follow would put the item on screen before the
     * scroll had moved anything and there would be nothing left to animate. */
    if (ownsWindow && snap != NULL) {
        bool    inItemView = overviewSnapshotInItemView(snap);
        uint8_t viewKind   = overviewSnapshotViewKind(snap);
        BYTE    viewTarget = overviewSnapshotViewTarget(snap);

        if (inItemView) {
            /* The watched square, whatever kind of item is on it. */
            int itemMX = 0, itemMY = 0;
            overviewSnapshotItemViewSquare(snap, &itemMX, &itemMY);
            float itemX = (float)itemMX + 0.5f;
            float itemY = (float)itemMY + 0.5f;

            if (!v->wasInItemView) {
                /* The way in. The flag goes back as it was on the way out, so
                 * a player who had panned off the tank gets that camera back. */
                v->followBeforeItemView = v->cam.follow;
                v->cam.follow = true;
                overviewCameraScrollTo(&v->cam, w, h, itemX, itemY);
            } else if (viewKind != v->wasViewKind ||
                       viewTarget != v->wasViewTarget) {
                /* A step to another item, of this kind or another. Follow is
                 * left alone — only the way in and the way out own it. */
                overviewCameraScrollTo(&v->cam, w, h, itemX, itemY);
            }
        } else if (v->wasInItemView) {
            /* The way out, by whatever route: the player leaving the view, the
             * watched item going, the decay clock running out, a death, a
             * respawn or the round resetting. They all read as the view kind
             * going back to the tank, so there is one exit to answer.
             *
             * The scroll back is for the free camera, which is the one that
             * would otherwise be left looking at empty ground. With follow on
             * the tank cannot stay off screen — following re-centres it — so a
             * scroll there would only be a delay in front of a centring that
             * jumps the rest of the way, since the scroll stops at the margin.
             *
             * Nothing to scroll to while the tank is dead — the flag is still
             * restored, and the follow below picks the tank up on the respawn. */
            v->cam.follow = v->followBeforeItemView;
            if (!v->cam.follow) {
                float tankX = 0.0f, tankY = 0.0f;
                if (overviewSnapshotTankPos(snap, &tankX, &tankY)) {
                    overviewCameraScrollToShow(&v->cam, w, h, tankX, tankY);
                }
            }
        }

        v->wasInItemView = inItemView;
        v->wasViewKind   = viewKind;
        v->wasViewTarget = viewTarget;
    }

    /* The respawn, on the same terms and for the same reason: it can start a
     * scroll, so it runs before the camera is settled. Outside the map test
     * below — a tank coming back is worth answering on a frame with no map to
     * draw, and the ring the tick starts is drawn on the frames that follow. */
    if (ownsWindow) {
        overviewViewTickRespawn(v, w, h, snap);
    }

    const OverviewMap *om = overviewSnapshotMap(snap);
    if (om != NULL && tiles != NULL) {
        if (sheetScale < 1) sheetScale = 1;
        /* The ImGui SDL3 backend sets the sampler per draw, so the host's
         * one-off setting at build time does not survive to here. Re-read the
         * player's texture filter rather than forcing NEAREST: the sheet is
         * the host's, and leaving it on NEAREST would hold the classic view
         * to that filter too. */
        SDL_SetTextureScaleMode(
            tiles, sdl3DrawScaleModeForFilter(gfxGetTextureFilter()));

        /* How long since the last frame, taken whether or not a scroll is
         * running: a scroll started after the view sat idle would otherwise
         * be handed the whole gap as its first step and finish instantly. */
        Uint64 nowTick = SDL_GetTicks();
        float  dtMs    = (v->scrollTick == 0)
                             ? 0.0f
                             : (float)(nowTick - v->scrollTick);
        v->scrollTick = nowTick;

        bool inItemView = ownsWindow && overviewSnapshotInItemView(snap);

        /* Four claims on the centre, in the order they win.
         *
         * A scroll in flight is the player being taken somewhere, so it has
         * the frame to itself — anything else moving the centre would leave
         * nothing to animate. Under it, an item view centres on what it is
         * watching, the same way the tank view centres on the tank, so an ally
         * stays in the middle of the picture instead of riding the edge they
         * were scrolled in over. With follow off — the player has panned away
         * — nothing moves. Under that, a fog mode that places the live block
         * from the classic view is followed on the block: it is what the
         * player is driving, and following the tank instead would leave the
         * block riding the edge of the picture. Under that again, the tank
         * view follows the tank, which is what a mode that centres its block
         * on the tank always does and what the rest fall back to with no block
         * to follow.
         *
         * A tank waiting to respawn has a position but is not anywhere the
         * player is, so follow mode holds the centre it already had. The
         * sub-square read is what lets follow glide with the tank rather
         * than stepping a whole square at a time; with Smooth Scrolling off
         * the position is snapped back to its square's centre, so follow
         * steps the way the classic view's scroll does. The block follow reads
         * and snaps the same way. */
        float tankX = 0.0f, tankY = 0.0f;
        float winX = 0.0f, winY = 0.0f;
        if (overviewCameraScrollTick(&v->cam, dtMs)) {
            /* The scroll has the centre this frame. */
        } else if (inItemView) {
            int itemMX = 0, itemMY = 0;
            overviewSnapshotItemViewSquare(snap, &itemMX, &itemMY);
            overviewCameraFollowTick(&v->cam, w, h,
                                     (float)itemMX + 0.5f,
                                     (float)itemMY + 0.5f);
        } else if (overviewSnapshotWindowCentre(snap, &winX, &winY)) {
            if (!smoothScrollingEnabled) {
                winX = SDL_floorf(winX) + 0.5f;
                winY = SDL_floorf(winY) + 0.5f;
            }
            overviewCameraFollowTick(&v->cam, w, h, winX, winY);
        } else if (overviewSnapshotTankPos(snap, &tankX, &tankY)) {
            if (!smoothScrollingEnabled) {
                tankX = SDL_floorf(tankX) + 0.5f;
                tankY = SDL_floorf(tankY) + 0.5f;
            }
            overviewCameraFollowTick(&v->cam, w, h, tankX, tankY);
        }

        int left = 0, top = 0, right = 0, bottom = 0;
        if (overviewCameraVisibleRange(&v->cam, w, h,
                                       &left, &top, &right, &bottom)) {
            overviewViewDrawTerrain(r, tiles, sheetScale, &v->cam, w, h, om,
                                    left, top, right, bottom);
            overviewViewDrawFog(v, r, &v->cam, w, h, om,
                                left, top, right, bottom);
        }

        /* Smart pings on the ground, on the same terms the classic view draws
         * them: after the terrain and the fog, before every sprite, so the
         * tanks and men a ping points at stay on top of it. One map square at
         * the current zoom, on the centre of the square the ping landed in.
         * A ping is a teammate telling the player where to look, so it is not
         * held to the live-square filter the sprites are: the point of it is
         * often ground nobody can see. */
        {
            const ClientPing *pl   = overviewSnapshotPings(snap);
            int               np   = overviewSnapshotPingCount(snap);
            Uint32            nowMs = (Uint32)SDL_GetTicks();
            float zoomScale = overviewCameraZoomScale(&v->cam);
            float tilePx = (float)OVERVIEW_TILE_PX * zoomScale;
            /* The sender's names, through this view's own cache and the main
             * window's face — the same arrangement, and the same scaling from
             * the face's own size to this view's zoom, the tank names use.
             *
             * Where they part company is the zoomed-out end. Below
             * OVERVIEW_LABEL_MIN_ZOOM the tank names are dropped, because a
             * map full of tanks at that size is a wall of unreadable text; a
             * ping is one of a handful and naming it is the whole point, so
             * instead of dropping it the size stops following the zoom down.
             * The name is then larger than the map around it, which is what a
             * player zoomed out to see the whole board wants. */
            int mainZoom = sdl3DrawGetZoomFactor();
            if (mainZoom < 1) mainZoom = 1;
            float nameZoom = (zoomScale > OVERVIEW_LABEL_MIN_ZOOM)
                           ? zoomScale : OVERVIEW_LABEL_MIN_ZOOM;
            PingMarkerLabel label;
            label.cache = &v->pingNameCache;
            label.font  = sdl3DrawGetMessageFont();
            label.scale = nameZoom / (float)mainZoom;
            for (int i = 0; i < np; i++) {
                Uint32 ageMs = nowMs - pl[i].recvMs;
                /* Expiry only -- pingMarkerDraw decides the blink and the
                   name's fade, the same way for every view that draws the
                   world marker. */
                float  alpha = pingDisplayAlpha((int)ageMs);
                float  sx = 0.0f, sy = 0.0f;
                if (nowMs < pl[i].recvMs || alpha <= 0.0f) continue;
                overviewCameraWorldToScreen(&v->cam, w, h,
                                            (float)(pl[i].worldX >> 8) + 0.5f,
                                            (float)(pl[i].worldY >> 8) + 0.5f,
                                            &sx, &sy);
                label.name = pl[i].senderName;
                label.slot = pl[i].sender;
                pingMarkerDraw(r, pl[i].kind, sx, sy, tilePx, tilePx,
                               ageMs, &label);
            }
        }

        /* Sprites on top of the fog: a tank only stands on a live square, and
         * the build cursor and the gunsight are the player's own marks, so
         * neither wants dimming. */
        overviewViewDrawEntities(v, r, tiles, sheetScale, crosshair, &v->cam,
                                 w, h, snap);
    }

    /* Only where this view has replaced the classic one: beside the pop-out
     * the 15x15 is still on screen with its own corner label, and the pop-out
     * is too small to give a border to. Before the blackout, so a death takes
     * it down with the rest of the picture. */
    if (ownsWindow && overviewSnapshotInItemView(snap)) {
        overviewViewDrawItemViewBorder(r, w, h);
    }

    /* Over the lot, and outside the test above: a death is worth answering
     * even on a frame with no map to draw. The host's own chrome — the status
     * panels in full screen, the window's furniture in the pop-out — goes on
     * afterwards and stays clear of it, so the player can still read what
     * they died with. */
    overviewViewDrawDeathBlackout(v, r, w, h, snap);

    /* And the way back out of one, after the blackout because the two are ends
     * of the same edge: the black stops being drawn on the frame the tank is
     * alive again and the spotlight and its ring start on it, over a map that
     * is fully back.
     *
     * Only where this view has replaced the classic one. Beside the pop-out
     * the player has the 15x15 in front of them, which re-centres on the tank
     * of its own accord, so there is nothing left to tell them. */
    if (ownsWindow) {
        overviewViewDrawRespawn(v, r, w, h, snap);
    }

    SDL_SetRenderTarget(r, NULL);
}

/* The overview's own keys as SDL scancodes — ImGui numbers its keys its own
 * way, and the bindings are scancodes. 0 for any key the overview does not
 * read, which keyIsClaimedByGame never claims. */
static int overviewScancodeForKey(ImGuiKey key) {
    switch (key) {
        case ImGuiKey_LeftArrow:      return SDL_SCANCODE_LEFT;
        case ImGuiKey_RightArrow:     return SDL_SCANCODE_RIGHT;
        case ImGuiKey_UpArrow:        return SDL_SCANCODE_UP;
        case ImGuiKey_DownArrow:      return SDL_SCANCODE_DOWN;
        case ImGuiKey_Home:           return SDL_SCANCODE_HOME;
        default:                      return 0;
    }
}

/* A key the player has bound to an in-game action belongs to the game: the
 * overview reads it as unpressed and does without it, rather than panning or
 * zooming while the same press also drives the tank. */
static bool overviewKeyPressed(const keyItems *keys, ImGuiKey key) {
    if (keyIsClaimedByGame(keys, overviewScancodeForKey(key))) return false;
    return ImGui::IsKeyPressed(key);
}

static bool overviewKeyDown(const keyItems *keys, ImGuiKey key) {
    if (keyIsClaimedByGame(keys, overviewScancodeForKey(key))) return false;
    return ImGui::IsKeyDown(key);
}

/* The modifier mask a scancode is one half of, or 0 for an ordinary key. Both
 * halves map to the pair: nobody rebinds to reach for one particular Ctrl, so
 * a binding on the left one answers to the right one too. */
static SDL_Keymod overviewModMaskFor(int scancode) {
    switch (scancode) {
        case SDL_SCANCODE_LCTRL:
        case SDL_SCANCODE_RCTRL:  return SDL_KMOD_CTRL;
        case SDL_SCANCODE_LSHIFT:
        case SDL_SCANCODE_RSHIFT: return SDL_KMOD_SHIFT;
        case SDL_SCANCODE_LALT:
        case SDL_SCANCODE_RALT:   return SDL_KMOD_ALT;
        case SDL_SCANCODE_LGUI:
        case SDL_SCANCODE_RGUI:   return SDL_KMOD_GUI;
        default:                  return SDL_KMOD_NONE;
    }
}

/* Does the wheel zoom this instant, or move the gunsight? The zoom key is the
 * view's own binding rather than one of the keys it borrows, so it is read
 * straight off the keyboard: keyIsClaimedByGame counts it — which is what
 * stops the overview reaching for the same key to pan or centre — and
 * overviewKeyDown would therefore always read it as up.
 *
 * A modifier is read through the mod state, which SDL keeps from key events,
 * rather than the keyboard array: input.c's mine key is on Shift for the same
 * reason — the array reports a held modifier on some platforms long after it
 * was let go, and here that would leave the wheel stuck on zoom with the
 * gunsight unreachable.
 *
 * Unbound means the wheel zooms the way it did before there was a key to hold,
 * so clearing the row in Key Setup is the way back to that. */
static bool overviewWheelZooms(const keyItems *keys) {
    int sc = keys ? keys->kiOverviewZoom : 0;
    if (sc <= 0) return true;
    if (sc >= SDL_SCANCODE_COUNT) return false;

    SDL_Keymod mask = overviewModMaskFor(sc);
    if (mask != SDL_KMOD_NONE) return (SDL_GetModState() & mask) != 0;

    const bool *state = SDL_GetKeyboardState(NULL);
    return state && state[sc];
}

/* An in-game binding held, read straight off the keyboard for the same reason
 * overviewWheelZooms is: keyIsClaimedByGame counts every keyItems field, so a
 * binding the view is deliberately taking over reads as up through
 * overviewKeyDown. Unbound is never held. */
static bool overviewBindingDown(int scancode) {
    if (scancode <= 0 || scancode >= SDL_SCANCODE_COUNT) return false;
    const bool *state = SDL_GetKeyboardState(NULL);
    return state && state[scancode];
}

/* The same read on the edge: true only on the frame the binding goes from up
 * to down, so one press is one action and holding the key does nothing more.
 * The keyboard array says what is down this instant and nothing about what was,
 * so the previous state is kept in the view.
 *
 * Every call updates that memory, so a caller has to ask once a frame for each
 * binding it reads — including on the frames it throws the answer away, or a
 * press made while it was not looking would fire as soon as it looked again.
 * Unbound is never pressed. */
static bool overviewBindingPressed(OverviewView *v, int scancode) {
    if (!v || scancode <= 0 || scancode >= SDL_SCANCODE_COUNT) return false;
    const bool *state = SDL_GetKeyboardState(NULL);
    bool down = (state != NULL) && state[scancode];
    bool was  = v->bindingWasDown[scancode];
    v->bindingWasDown[scancode] = down;
    return down && !was;
}

/* The map square under the mouse pointer, taken against the item the caller
 * submitted immediately before this — the same rect the wheel zoom anchors
 * to. False when the pointer is off the map; the square is still written, so
 * callers have to test the return rather than the values. */
static bool overviewSquareUnderMouse(const OverviewCamera *cam,
                                     int viewW, int viewH,
                                     int *outMapX, int *outMapY) {
    ImVec2 rectMin = ImGui::GetItemRectMin();
    ImVec2 mouse   = ImGui::GetMousePos();
    return overviewCameraScreenToWorld(cam, viewW, viewH,
                                       mouse.x - rectMin.x, mouse.y - rectMin.y,
                                       outMapX, outMapY);
}

extern "C" void overviewViewHandleInput(OverviewView *v, bool hovered,
                                        int viewW, int viewH, ClientSim *cs,
                                        const keyItems *keys, bool ownsWindow) {
    if (!v || viewW <= 0 || viewH <= 0) return;

    ImGuiIO &io = ImGui::GetIO();
    OverviewCamera *cam = &v->cam;

    /* The pan InvisibleButton the caller submitted immediately before this
     * owns the press, so a drag that wanders off the image keeps panning. */
    if (ImGui::IsItemActive()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        /* Only a press that actually moved pans — a plain click would
         * otherwise pan by nothing and switch follow off as a side effect.
         * Negated: dragging right pulls the map right, which is the centre
         * moving left. */
        if (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f) {
            overviewCameraPan(cam, viewW, viewH,
                              -io.MouseDelta.x, -io.MouseDelta.y);
        }
    }

    /* The game's crosshair while the pointer is over the map, the system
     * cursor back when it leaves. The ImGui SDL3 backend only calls
     * SDL_SetCursor when its own expected cursor changes, so one set here
     * holds frame after frame — but the drag above asks for ResizeAll, and
     * ImGui going back to Arrow once the drag ends is such a change, applied
     * at the start of the frame after. Setting the crosshair before that
     * would be wiped, so the drag suppresses it for one frame more and the
     * set lands the frame after, when the backend has already been through.
     * The restore is this view's own job: cursorMove only tracks the main
     * window's pointer and will not undo a crosshair set from here. */
    bool dragging = ImGui::IsItemActive();
    if (dragging || v->dragWasActive) {
        v->crosshairOn = false;
    } else if (hovered) {
        if (!v->crosshairOn) {
            cursorSetCursor(false);
            v->crosshairOn = true;
        }
    } else if (v->crosshairOn) {
        cursorSetCursor(true);
        v->crosshairOn = false;
    }
    v->dragWasActive = dragging;

    /* The wheel moves the gunsight over the map exactly as it does over the
     * main view, and zooms only while the zoom key is held. The bump is
     * dispatched from here rather than left to the wheel handler in
     * sdl3imgui.cpp, which neither host's wheel reaches: the in-window mode
     * sits under an ImGui window that captures the mouse, and the pop-out's
     * events are consumed as that window's own. Same running-game test the
     * handler there makes.
     *
     * Not while the smart-ping pie is open, for the reason the gunsight bump
     * in sdl3imgui.cpp is not either: the wheel is under the same hand that is
     * holding the menu, and a nudge while choosing a ping should not re-zoom
     * the map out from under the square the ping is already aimed at. */
    if (hovered && io.MouseWheel != 0.0f && !pingOverlayIsMenuOpen()) {
        int steps = (io.MouseWheel > 0.0f) ? 1 : -1;
        if (overviewWheelZooms(keys)) {
            ImVec2 rectMin = ImGui::GetItemRectMin();
            ImVec2 mouse   = ImGui::GetMousePos();
            overviewCameraZoomAt(cam, viewW, viewH,
                                 mouse.x - rectMin.x, mouse.y - rectMin.y, steps);
        } else if (cs && clientSimGetNetStatus(cs) == netRunning) {
            inputBumpGunsight(steps);
        }
    }

    /* Moving the pointer over the map drives the one shared build cursor, the
     * way moving it over the main view does, so toggling build mode picks up
     * where the pointer is. Only on real movement, and only while the pointer
     * is over a square that exists — past the map edge it stays where it was
     * rather than jumping to a clamped square.
     *
     * While cursor mode is ON, buildCursorClampToView drags the cursor back
     * inside the main view's 15x15 each frame, so a square set from here
     * outside that area does not survive to the next one. Cursor mode is the
     * keyboard/gamepad affordance; with a mouse it is off and the clamp is a
     * no-op. The click below dispatches from the pointer's square rather than
     * from the cursor, so building is unaffected either way. */
    if (hovered && (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f)) {
        int mx = 0, my = 0;
        if (overviewSquareUnderMouse(cam, viewW, viewH, &mx, &my)) {
            buildCursorSetTile((BYTE)mx, (BYTE)my);
        }
    }

    /* Tested every frame, not just on movement: the flag has to drop the
     * instant the pointer leaves the map so the square fades back to the
     * locked-target rendering. */
    {
        int mx = 0, my = 0;
        v->mouseOnMap = hovered &&
                        overviewSquareUnderMouse(cam, viewW, viewH, &mx, &my);
    }

    /* Left-click builds at the square under the pointer. The pan item claims
     * the right button, so the left one arrives here unswallowed. Range,
     * terrain and cost are the server's to refuse, exactly as for a build
     * dispatched from the main view; clientSimManMoveToMap already drops the
     * request for a dead tank or a failed connection. */
    if (hovered && cs && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        int mx = 0, my = 0;
        if (overviewSquareUnderMouse(cam, viewW, viewH, &mx, &my)) {
            clientMutexWaitFor();
            clientSimManMoveToMap(cs, (BYTE)mx, (BYTE)my,
                                  clientSimGetCurrentBuildSelect(cs));
            clientMutexRelease();
            /* Leave the shared cursor on the square just built, as pointer
             * motion over the main view would have. */
            buildCursorSetTile((BYTE)mx, (BYTE)my);
        }
    }

    /* The view's own camera bindings — zoom in, zoom out and the follow
     * toggle. Read straight off the keyboard for the reason
     * overviewBindingPressed gives, and read here whatever happens next so
     * the edge behind them stays current while a text box has the keyboard;
     * what a text box changes is whether they are acted on. */
    bool zoomInPressed  = overviewBindingPressed(v, keys ? keys->kiOverviewZoomIn  : 0);
    bool zoomOutPressed = overviewBindingPressed(v, keys ? keys->kiOverviewZoomOut : 0);
    bool followPressed  = overviewBindingPressed(v, keys ? keys->kiOverviewFollow  : 0);

    /* Keyboard zoom has no cursor to hold onto, so it anchors the centre. */
    float midX = (float)viewW * 0.5f;
    float midY = (float)viewH * 0.5f;
    if (!io.WantTextInput) {
        if (zoomInPressed) {
            overviewCameraZoomAt(cam, viewW, viewH, midX, midY, 1);
        }
        if (zoomOutPressed) {
            overviewCameraZoomAt(cam, viewW, viewH, midX, midY, -1);
        }
        /* Following on again wants no centring of its own: the follow tick
         * brings the tank back on the next frame. Off leaves the camera
         * exactly where the player has it, which is the one thing panning
         * could not give them. */
        if (followPressed) {
            cam->follow = !cam->follow;
        }
    }

    /* The keyboard's pan. The arrows are the view's own and are given up to
     * whatever in-game binding has taken them; the scroll keys are added while
     * this view owns the window, because the classic view they would otherwise
     * scroll is not on screen — and on the default bindings the scroll keys
     * *are* the arrows, so without this there is no keyboard pan at all. The
     * two sets never both report the same key: a bound one is claimed and
     * reads as up through overviewKeyDown, and only a bound one is read by
     * overviewBindingDown.
     *
     * Not while an item view has them: there the scroll keys step between
     * pills, bases or allied tanks, which is still their job with the classic
     * view hidden. Not while a text box has the keyboard either, or typing a
     * message would pan the map behind it. Not under an overview window that
     * places the live block from the classic view either: there the scroll
     * keys are what drags the block, and the classic scroll wants them back.
     *
     * Panning clears follow, the way a drag does, so a held key wins over the
     * tank exactly as manual scrolling wins over auto-scroll in the classic
     * view. Home hands the map back to the tank. */
    if (!io.WantTextInput) {
        bool scrollKeysArePan = ownsWindow && cs && !clientSimIsInItemView(cs) &&
                                !clientSimOverviewWindowFollowsView(cs);
        float dx = 0.0f, dy = 0.0f;
        if (overviewKeyDown(keys, ImGuiKey_LeftArrow))  dx -= OVERVIEW_ARROW_STEP_PX;
        if (overviewKeyDown(keys, ImGuiKey_RightArrow)) dx += OVERVIEW_ARROW_STEP_PX;
        if (overviewKeyDown(keys, ImGuiKey_UpArrow))    dy -= OVERVIEW_ARROW_STEP_PX;
        if (overviewKeyDown(keys, ImGuiKey_DownArrow))  dy += OVERVIEW_ARROW_STEP_PX;
        if (scrollKeysArePan && keys) {
            if (overviewBindingDown(keys->kiScrollLeft))  dx -= OVERVIEW_ARROW_STEP_PX;
            if (overviewBindingDown(keys->kiScrollRight)) dx += OVERVIEW_ARROW_STEP_PX;
            if (overviewBindingDown(keys->kiScrollUp))    dy -= OVERVIEW_ARROW_STEP_PX;
            if (overviewBindingDown(keys->kiScrollDown))  dy += OVERVIEW_ARROW_STEP_PX;
        }
        if (dx != 0.0f || dy != 0.0f) {
            overviewCameraPan(cam, viewW, viewH, dx, dy);
        }
    }

    /* Nothing to centre on while the tank is dead, so the key does nothing
     * rather than throwing the view at wherever the corpse reads. */
    if (overviewKeyPressed(keys, ImGuiKey_Home)) {
        float tankX = 0.0f, tankY = 0.0f;
        if (clientSimIsMyTankAlive(cs) &&
            clientSimGetMyTankMapPosF(cs, &tankX, &tankY)) {
            overviewCameraCenterOnTank(cam, viewW, viewH, tankX, tankY);
        }
    }
}

/* The hover test in overviewViewHandleInput restores the pointer when it
 * leaves the map, but hiding the window with the pointer still over it stops
 * that function running at all, and the crosshair would be left set over the
 * rest of the UI until the main view's next in/out transition. */
extern "C" void overviewViewReleaseCursor(OverviewView *v) {
    if (v && v->crosshairOn) {
        cursorSetCursor(true);
        v->crosshairOn = false;
    }
}
