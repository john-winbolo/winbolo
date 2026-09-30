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
 * Name:          imgui_dialog_utils.h
 * Purpose:       Shared utility for ImGui dialog scaling.
 *                Computes a UI scale factor from screen
 *                size, loads a scaled font, and applies
 *                touch-friendly styling when needed.
 *                Works on desktop, Android, and Steam Deck.
 *********************************************************/

#ifndef IMGUI_DIALOG_UTILS_H
#define IMGUI_DIALOG_UTILS_H

#include <SDL3/SDL.h>
#include "imgui.h"
#include "imgui_nav_outline.h"
#include "dialog_quit.h"
#include "../../imgui_fonts.h"
#include "../../../common/wb_log.h"
#include "nanosvg.h"
#include "nanosvgrast.h"
/* Tile-atlas geometry for imguiDrawTileIcon() below. tiles.h is a
 * dependency-free constants header, so it costs the many TUs that include
 * this one nothing beyond the macros themselves. */
#include "../../tiles.h"

/* Pulled in early so dialogApplyScaling() below can branch on Deck mode.
 * (Same file also re-includes near s_devicePresets — that's fine, the
 * header has its own guard.) */
#ifdef __cplusplus
extern "C" {
#endif
#include "../../ui_mode.h"
#ifdef __cplusplus
}
#endif

/* ImGui-side UI scale on Deck.  Decoupled from the SDL_ttf integer zoom
 * factor (which the in-game HUD uses for crisp bitmap rasterisation) —
 * ImGui doesn't benefit from integer scaling, and 2x produced 40pt fonts
 * + 2x layout that overflowed Deck's 1280x800 surface.  1.5x gives ~30pt
 * dialog fonts and 1.5x layout, which is the right handheld viewing
 * scale at ~14 inches and lets the existing `panelW = X * s` patterns
 * fit without per-dialog tweaking.  Returns 1 elsewhere. */
static inline float dialogDeckFontMul(void) {
    return uiModeIsSteamDeck() ? 1.5f : 1.0f;
}

/* Unified mobile platform check — use BOLO_MOBILE instead of
 * repeating __ANDROID__ || __IPHONEOS__ everywhere. */
#if defined(__ANDROID__) || defined(__IPHONEOS__)
#define BOLO_MOBILE 1
#else
#define BOLO_MOBILE 0
#endif

#ifdef __APPLE__
  #define KMOD_PRIMARY        SDL_KMOD_GUI
  #define KMOD_PRIMARY_LABEL  "Cmd+"
#else
  #define KMOD_PRIMARY        SDL_KMOD_CTRL
  #define KMOD_PRIMARY_LABEL  "Ctrl+"
#endif

#ifdef __APPLE__
  #define IMGUI_PRIMARY_KEY_DOWN()  (ImGui::GetIO().KeySuper)
#else
  #define IMGUI_PRIMARY_KEY_DOWN()  (ImGui::GetIO().KeyCtrl)
#endif

/* Ping readout colours, keyed off the band rather than re-deriving
 * thresholds per call site (they had drifted into six copies of the same
 * ladder). PING_BAND_NONE is the "---" grey; in-game rows get their band
 * from clientSimGetPlayerPingBand (hysteresis-tracked), the lobby from
 * pingBandClassify. */
#include "ping_display.h"

static inline ImVec4 imguiPingBandColor(PingBand band) {
    switch (band) {
        case PING_BAND_GOOD: return ImVec4(0.0f, 0.9f, 0.0f, 1.0f);
        case PING_BAND_FAIR: return ImVec4(0.9f, 0.9f, 0.0f, 1.0f);
        case PING_BAND_POOR: return ImVec4(0.9f, 0.0f, 0.0f, 1.0f);
        case PING_BAND_NONE:
        default:             return ImVec4(0.5f, 0.5f, 0.5f, 1.0f);
    }
}

/* Prevent iOS from shifting the entire SDL view when the soft keyboard appears.
 * SDL3's iOS view controller monitors the textInputRect set via
 * SDL_SetTextInputArea() and scrolls the view so the text field stays visible.
 * We don't want that — instead we leave the view in place and let the keyboard
 * overlay on top.  Call this after ImGui_ImplSDL3_NewFrame() (which sets the
 * text input area) to reset it so the view is never shifted.
 *
 * Also call SDL_StopTextInput() to dismiss the keyboard when a dialog closes
 * or the user taps outside a text field. */
#if defined(__IPHONEOS__)
static inline void dialogResetTextInputArea(SDL_Window *window) {
    SDL_Rect r = {0, 0, 1, 0};
    SDL_SetTextInputArea(window, &r, 0);
}

static inline void dialogDismissKeyboard(SDL_Window *window) {
    if (SDL_TextInputActive(window)) {
        SDL_StopTextInput(window);
    }
}
#else
static inline void dialogResetTextInputArea(SDL_Window *window) { (void)window; }
static inline void dialogDismissKeyboard(SDL_Window *window) { (void)window; }
#endif

/* Compute UI scale factor from window dimensions.
 * On desktop the dialogs scale with the window height (reference 1080px,
 * clamped to [1.0, 2.5]) so they stay readable on large / 4K windows;
 * desktop tablet mode keeps 1.0 and scales via its own path.
 * On Android (and similar full-screen platforms) the dialog renders into
 * the device's full window, so we scale based on screen height.
 * Reference height is 540px (1x scale).
 * Steam Deck multiplies the result by the SDL_ttf zoom factor so every
 * `panelW = X * s` / `Button(..., ImVec2(W * s, ...))` pattern across the
 * dialog code automatically scales to match the bumped ImGui font. */
static inline float dialogComputeScale(int screenW, int screenH) {
    (void)screenW;
    float scale;
#if BOLO_MOBILE
    scale = (float)screenH / 540.0f;
    if (scale < 1.0f) scale = 1.0f;
#else
    if (uiModeIsTablet()) {
        (void)screenH;
        scale = 1.0f;          /* tablet mode scales via its own path */
    } else if (!uiModeIsSteamDeck() && uiUiScaleGet() != UI_SCALE_AUTO) {
        /* Desktop with a UI-scale override: pin to the preset factor
           instead of the window-height-derived scale. */
        scale = uiUiScalePresetFactor(uiUiScaleGet());
    } else {
        scale = (float)screenH / 1080.0f;   /* desktop: scale to window height */
        if (scale < 1.0f) scale = 1.0f;
        if (scale > 2.5f) scale = 2.5f;
    }
#endif
    if (uiModeIsSteamDeck()) {
        scale *= dialogDeckFontMul();
    }
    return scale;
}

/* Load font data via SDL_IOFromFile (works on Android assets and desktop).
 * Returns buffer allocated with IM_ALLOC (ImGui takes ownership). */
static inline unsigned char *dialogLoadFontData(const char *path, int *outSize) {
    SDL_IOStream *io = SDL_IOFromFile(path, "rb");
    if (!io) return nullptr;
    Sint64 size = SDL_GetIOSize(io);
    if (size <= 0) { SDL_CloseIO(io); return nullptr; }
    unsigned char *buf = (unsigned char *)IM_ALLOC((size_t)size);
    if (!buf) { SDL_CloseIO(io); return nullptr; }
    size_t bytesRead = SDL_ReadIO(io, buf, (size_t)size);
    SDL_CloseIO(io);
    if ((Sint64)bytesRead != size) { IM_FREE(buf); return nullptr; }
    *outSize = (int)size;
    return buf;
}

/* Rasterise an SVG file at the given pixel size and upload it as an
 * SDL_Texture. Returns NULL on parse failure, missing file, or zero-
 * dimension SVG. Caller owns the returned texture and is responsible
 * for SDL_DestroyTexture() if cleanup is desired (most call sites
 * cache for app lifetime). The output is always size×size; the SVG
 * is scaled to fit while preserving aspect ratio and centred. */
static inline SDL_Texture *imguiLoadSvgIcon(SDL_Renderer *rend, const char *path, int size) {
    NSVGimage *image = nsvgParseFromFile(path, "px", 96.0f);
    if (!image) return nullptr;
    if (image->width < 1.0f || image->height < 1.0f) { nsvgDelete(image); return nullptr; }
    float scale = (float)size / image->height;
    if (image->width * scale > (float)size) scale = (float)size / image->width;
    int w = size, h = size;
    unsigned char *pixels = (unsigned char *)SDL_malloc((size_t)(w * h * 4));
    if (!pixels) { nsvgDelete(image); return nullptr; }
    memset(pixels, 0, (size_t)(w * h * 4));
    float offX = ((float)w - image->width * scale) * 0.5f;
    float offY = ((float)h - image->height * scale) * 0.5f;
    NSVGrasterizer *rast = nsvgCreateRasterizer();
    nsvgRasterize(rast, image, offX, offY, scale, pixels, w, h, w * 4);
    nsvgDeleteRasterizer(rast);
    nsvgDelete(image);
    SDL_Surface *surface = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA32, pixels, w * 4);
    if (!surface) { SDL_free(pixels); return nullptr; }
    SDL_Texture *tex = SDL_CreateTextureFromSurface(rend, surface);
    SDL_DestroySurface(surface);
    SDL_free(pixels);
    return tex;
}

/* Rasterise an SVG to an RGBA32 surface the caller owns. `whiten` forces the
 * colour channels to white, leaving the alpha as the only information — for a
 * monochrome badge that has to read against an arbitrary background and be
 * tinted at draw time. Without it the artwork keeps its own colours, which is
 * what multi-colour art needs: a tint can darken a texture but never brighten
 * it, so a whitened chip cannot be turned back into a chip.
 *
 * One body for both because the two differ by that loop alone; they were
 * copies of each other, which is two places to fix a rasterisation bug in. */
static inline SDL_Surface *imguiRasterizeSvgSurface(const char *path, int size,
                                                    bool whiten) {
    NSVGimage *image = nsvgParseFromFile(path, "px", 96.0f);
    if (!image) return nullptr;
    if (image->width < 1.0f || image->height < 1.0f) { nsvgDelete(image); return nullptr; }
    float scale = (float)size / image->height;
    if (image->width * scale > (float)size) scale = (float)size / image->width;
    int w = size, h = size;
    SDL_Surface *surface = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGBA32);
    if (!surface) { nsvgDelete(image); return nullptr; }
    memset(surface->pixels, 0, (size_t)surface->pitch * (size_t)h);
    float offX = ((float)w - image->width * scale) * 0.5f;
    float offY = ((float)h - image->height * scale) * 0.5f;
    NSVGrasterizer *rast = nsvgCreateRasterizer();
    nsvgRasterize(rast, image, offX, offY, scale,
                  (unsigned char *)surface->pixels, w, h, surface->pitch);
    nsvgDeleteRasterizer(rast);
    nsvgDelete(image);
    if (whiten) {
        for (int yy = 0; yy < h; ++yy) {
            unsigned char *row = (unsigned char *)surface->pixels + (size_t)yy * (size_t)surface->pitch;
            for (int xx = 0; xx < w; ++xx) {
                row[xx * 4 + 0] = 255;
                row[xx * 4 + 1] = 255;
                row[xx * 4 + 2] = 255;
            }
        }
    }
    return surface;
}

/* imguiLoadSvgIcon as a SURFACE, with the artwork's own colours kept. For
 * renderer-independent caches — the tank label textures its icons per
 * renderer, so it needs a surface, and the bot chip it draws is multi-colour
 * artwork that a white mask would flatten. */
static inline SDL_Surface *imguiLoadSvgIconSurface(const char *path, int size) {
    return imguiRasterizeSvgSurface(path, size, false);
}

/* The same, as an alpha mask with the colour channels forced white. The
 * surface form exists for renderer-independent caches; most call sites want
 * the imguiLoadSvgIconWhite texture wrapper below. */
static inline SDL_Surface *imguiLoadSvgIconWhiteSurface(const char *path, int size) {
    return imguiRasterizeSvgSurface(path, size, true);
}

/* Force-white SVG rasterisation as a texture on the given renderer. Use for
 * monochrome icons that must read against an arbitrary ImGui background
 * regardless of the SVG's authored fill colour; ImGui's tint multiplier can
 * darken but cannot brighten, so dark SVG fills are unrecoverable without
 * this. */
static inline SDL_Texture *imguiLoadSvgIconWhite(SDL_Renderer *rend, const char *path, int size) {
    SDL_Surface *surface = imguiLoadSvgIconWhiteSurface(path, size);
    if (!surface) return nullptr;
    SDL_Texture *tex = SDL_CreateTextureFromSurface(rend, surface);
    SDL_DestroySurface(surface);
    return tex;
}

/* Like imguiLoadSvgIconWhite, but fits to the icon's *artwork* bounds rather
 * than the SVG's declared page size. Many icon SVGs (e.g. the Discord mark)
 * don't fill their square viewBox — the art is short and wide and floats in
 * the canvas — so a plain page-fit renders them small, off-centre and visually
 * lighter than icons whose art fills the box (e.g. Reddit). This centres the
 * union of the drawn shapes and scales it to fill `size` (less a small margin
 * so edge strokes aren't clipped), giving every icon a consistent weight. */
static inline SDL_Texture *imguiLoadSvgIconWhiteFit(SDL_Renderer *rend, const char *path, int size) {
    NSVGimage *image = nsvgParseFromFile(path, "px", 96.0f);
    if (!image) return nullptr;

    float minx = 1e30f, miny = 1e30f, maxx = -1e30f, maxy = -1e30f;
    bool any = false;
    for (NSVGshape *sh = image->shapes; sh; sh = sh->next) {
        if (!(sh->flags & NSVG_FLAGS_VISIBLE)) continue;
        if (sh->bounds[0] < minx) minx = sh->bounds[0];
        if (sh->bounds[1] < miny) miny = sh->bounds[1];
        if (sh->bounds[2] > maxx) maxx = sh->bounds[2];
        if (sh->bounds[3] > maxy) maxy = sh->bounds[3];
        any = true;
    }
    float cw = maxx - minx, ch = maxy - miny;
    if (!any || cw < 1e-3f || ch < 1e-3f) { nsvgDelete(image); return nullptr; }

    const float margin = 0.08f;                       /* keep strokes off the edge */
    float avail = (float)size * (1.0f - 2.0f * margin);
    float scale = avail / (cw > ch ? cw : ch);
    float offX  = ((float)size - cw * scale) * 0.5f - minx * scale;
    float offY  = ((float)size - ch * scale) * 0.5f - miny * scale;

    int w = size, h = size;
    unsigned char *pixels = (unsigned char *)SDL_malloc((size_t)(w * h * 4));
    if (!pixels) { nsvgDelete(image); return nullptr; }
    memset(pixels, 0, (size_t)(w * h * 4));
    NSVGrasterizer *rast = nsvgCreateRasterizer();
    nsvgRasterize(rast, image, offX, offY, scale, pixels, w, h, w * 4);
    nsvgDeleteRasterizer(rast);
    nsvgDelete(image);
    for (int i = 0; i < w * h; ++i) {
        pixels[i * 4 + 0] = 255;
        pixels[i * 4 + 1] = 255;
        pixels[i * 4 + 2] = 255;
    }
    SDL_Surface *surface = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA32, pixels, w * 4);
    if (!surface) { SDL_free(pixels); return nullptr; }
    SDL_Texture *tex = SDL_CreateTextureFromSurface(rend, surface);
    SDL_DestroySurface(surface);
    SDL_free(pixels);
    return tex;
}

/* Draw a filled shield that appears to spin about its vertical axis, straight
 * into an ImDrawList — no texture, so it stays crisp at any size and takes
 * whatever colour you pass. Call once per frame with an advancing `phase`
 * (radians) to animate; e.g. phase = SDL_GetTicks() * 0.002f.
 *
 *   center - shield centre, screen-space pixels
 *   halfW  - half width at face-on, pixels (shield is taller than wide)
 *   halfH  - half height, pixels
 *   phase  - rotation angle; increases over time to spin
 *   col    - base colour (alpha honoured; shaded relative to it)
 *   filled - true draws the lit/dim fill plate; false draws the rim only
 *            (a hollow outline shield that still spins)
 *
 * The 3-D feel comes from two things: the width scales with cos(phase) so the
 * shield narrows to an edge-on sliver and swings back, and the fill is
 * lightened head-on / darkened toward edge-on so it reads as a solid plate
 * turning in the light rather than a flat shape being squashed. */
static inline void imguiDrawSpinningShield(ImDrawList *dl, ImVec2 center,
                                           float halfW, float halfH,
                                           float phase, ImU32 col,
                                           bool filled) {
    if (!dl || halfW < 1.0f || halfH < 1.0f) return;

    /* Shield outline, normalised to x,y in [-1,1] (x right, y down): flat
     * top, short vertical shoulders, curved sides to a bottom point. Convex,
     * so AddConvexPolyFilled fills it directly. */
    static const ImVec2 unit[] = {
        {-0.92f, -1.00f}, { 0.92f, -1.00f}, { 0.92f, -0.10f},
        { 0.80f,  0.30f}, { 0.58f,  0.62f}, { 0.30f,  0.86f}, { 0.00f, 1.00f},
        {-0.30f,  0.86f}, {-0.58f,  0.62f}, {-0.80f,  0.30f}, {-0.92f, -0.10f},
    };
    const int n = (int)(sizeof(unit) / sizeof(unit[0]));

    const float cosP = SDL_cosf(phase);
    const float face = SDL_fabsf(cosP);          /* 1 head-on … 0 edge-on */
    const float w    = halfW * cosP;             /* signed; |w|→0 edge-on   */

    const int   baseA = (int)((col >> IM_COL32_A_SHIFT) & 0xFF);
    const ImU32 rgb   = col & ~IM_COL32_A_MASK;

    ImVec2 pts[16];
    for (int i = 0; i < n; ++i) {
        pts[i].x = center.x + unit[i].x * w;
        pts[i].y = center.y + unit[i].y * halfH;
    }

    /* Lit face / dim edge gives the turning-in-the-light read. */
    if (filled) {
        ImU32 fill = rgb | ((ImU32)(baseA * (0.32f + 0.68f * face)) << IM_COL32_A_SHIFT);
        dl->AddConvexPolyFilled(pts, n, fill);
    }
    /* Full-alpha rim keeps the silhouette crisp; at edge-on it collapses to a
     * vertical line — exactly the shield seen on edge. */
    dl->AddPolyline(pts, n, col, ImDrawFlags_Closed, 1.4f);
}

/* Inline filled WBN shield badge, laid out like an ImGui::Image of
 * size×size px: it reserves the square slot (so SameLine and any icon-width
 * budgets stay unchanged) and centres the vector shield in it. Vector-drawn
 * via imguiDrawSpinningShield (face-on, filled) so it stays crisp at small
 * sizes and is the exact same silhouette — rim and all — as the hollow
 * sign-in shield, rather than a blurry rasterised SVG. `col` is the
 * fill/rim colour (white, or the supporter gold tint). */
static inline void imguiShieldBadge(float size, ImU32 col) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    float halfH = size * 0.5f;
    imguiDrawSpinningShield(ImGui::GetWindowDrawList(),
                            ImVec2(p.x + halfH, p.y + halfH),
                            halfH * 0.80f, halfH, 0.0f, col, true);
    ImGui::Dummy(ImVec2(size, size));
}

/* Open a URL in the system browser. Returns true on success.
 * SDL_OpenURL handles per-platform dispatch (ShellExecuteW on Windows,
 * xdg-open / open on Linux/macOS, etc.) — no need for our own #ifdef. */
static inline bool imguiOpenUrl(const char *url) {
    if (!url || !*url) return false;
    return SDL_OpenURL(url);
}

/* Switch to the hand cursor when the most-recently-submitted ImGui item is
 * hovered. Call immediately after a Button/SmallButton/ImageButton/
 * ArrowButton or a row-style Selectable. Safe to call on any frame — if the
 * item is not hovered, this is a no-op.
 *
 * Use the convention: clickable buttons and row selectables get the hand
 * cursor. Skip MenuItem/Checkbox/RadioButton (they have their own
 * affordances) and dropdown-list Selectables (the popup already implies
 * clickability). ImGui::TextLinkOpenURL() sets the cursor itself, so don't
 * follow it with this call. */
static inline void imguiHandOnHover(void) {
    if (ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
}

/* Show a one-line tooltip on mouse hover OR gamepad/keyboard focus, so
   controller users (who can't hover) still get it. Call right after the
   item whose tooltip this is.

   One test covers both: while the player is on the stick or the keys, ImGui
   counts the navigated item as the hovered one, and _ForTooltip picks the
   delay to match whichever of the two is driving. Asking IsItemFocused() on
   top of that only ever adds the case nobody wants — a mouse click leaves its
   target focused, which would pin the tooltip up, trailing the pointer around
   the window, until something else was clicked. */
static inline void imguiHelpTooltip(const char *text) {
    if (!text) return;
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip("%s", text);
}

/* ── Per-image texture sampling ─────────────────────────────────────
 * SDL_SetTextureScaleMode() on a texture is IGNORED for anything drawn
 * through ImGui::Image: the SDL_Renderer backend overwrites the scale
 * mode of every texture it binds, every draw command, from its own
 * per-frame state —
 *
 *     SDL_SetTextureScaleMode(tex, bd->CurrentScaleMode);
 *
 * (imgui_impl_sdlrenderer3.cpp) — and that state is reset to
 * SDL_SCALEMODE_LINEAR at the top of every render pass. The supported
 * way to get point sampling is the backend's standard sampler draw
 * callbacks, which it publishes in the platform IO.
 *
 * Bracket a magnified pixel-art Image with these: tile art blown up
 * several times over turns to mush under bilinear, and everything drawn
 * after it (glyphs especially) needs LINEAR back. Both no-op when the
 * active backend publishes no callbacks. */
/* The pair on a named list, for art that is not drawn into the current
 * window's list — the tablet HUD builds its buttons on the foreground
 * list, and a callback added to the window list would not bracket them. */
static inline void imguiPushNearestSamplingOn(ImDrawList *dl) {
    ImDrawCallback cb = ImGui::GetPlatformIO().DrawCallback_SetSamplerNearest;
    if (cb && dl) dl->AddCallback(cb, NULL);
}
static inline void imguiPopNearestSamplingOn(ImDrawList *dl) {
    ImDrawCallback cb = ImGui::GetPlatformIO().DrawCallback_SetSamplerLinear;
    if (cb && dl) dl->AddCallback(cb, NULL);
}
static inline void imguiPushNearestSampling(void) {
    imguiPushNearestSamplingOn(ImGui::GetWindowDrawList());
}
static inline void imguiPopNearestSampling(void) {
    imguiPopNearestSamplingOn(ImGui::GetWindowDrawList());
}

/* The game's tile atlas, owned by the SDL3 renderer. Declared here rather
 * than including sdl3draw.h, which would pull the whole render/sim surface
 * into every dialog translation unit. Non-game builds that include this
 * header never call the helpers below, so the declaration costs them
 * nothing. */
#ifdef __cplusplus
extern "C" {
#endif
SDL_Texture *sdl3DrawGetTilesTexture(void);
#ifdef __cplusplus
}
#endif

/* Draws a sprite from the game atlas inline at text height, used to mark a
 * table column with the map art for the thing it counts. The width follows
 * the source aspect, so a sprite that is not square — the 3x4 LGM — keeps
 * its proportions instead of being stretched. Source coords and extents are
 * in 1x units; the atlas is assembled at gSheetScale, but UVs normalised
 * against the 1x reference size (TILE_FILE_X/Y) stay correct at any scale. */
static inline void imguiDrawAtlasIcon(int srcX, int srcY, int srcW, int srcH) {
    SDL_Texture *tex = sdl3DrawGetTilesTexture();
    if (!tex || srcW <= 0 || srcH <= 0) return;
    float h = ImGui::GetTextLineHeight();
    float w = h * (float)srcW / (float)srcH;
    ImVec2 uv0((float)srcX / TILE_FILE_X, (float)srcY / TILE_FILE_Y);
    ImVec2 uv1((float)(srcX + srcW) / TILE_FILE_X,
               (float)(srcY + srcH) / TILE_FILE_Y);
    /* This is the game's live sheet, not a copy, and the backend leaves
       whatever sampler it bound with on it.  Without the bracket, one dialog
       carrying an icon leaves the map itself drawing through LINEAR for the
       rest of the session; sdl3DrawAssertTilesSampler puts it back each
       frame, and this keeps it from going wrong in the first place — the
       icons are pixel art at text height and want point sampling anyway. */
    imguiPushNearestSampling();
    ImGui::Image((ImTextureID)tex, ImVec2(w, h), uv0, uv1);
    imguiPopNearestSampling();
}

/* A whole 16x16 map tile — the common case, square at text height. */
static inline void imguiDrawTileIcon(int tileX, int tileY) {
    imguiDrawAtlasIcon(tileX, tileY, TILE_SIZE_X, TILE_SIZE_Y);
}

/* Builds a table header cell whose label is preceded by an inline map
 * sprite (icon to the left of the text). */
static inline void imguiDrawIconHeader(int tileX, int tileY, const char *label) {
    imguiDrawTileIcon(tileX, tileY);
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::TableHeader(label);
}

/* Register Platform_OpenInShellFn on the current ImGui context so that
 * ImGui::TextLinkOpenURL() actually launches the system browser on click.
 * Call once per ImGui::CreateContext(), with that context current. */
static inline void imguiRegisterPlatformOpenUrl(void) {
    ImGui::GetPlatformIO().Platform_OpenInShellFn =
        [](ImGuiContext *, const char *url) -> bool {
            return imguiOpenUrl(url);
        };
}

/* Render wrapped text with embedded http(s):// URLs auto-linked.
 * Each URL becomes a hand-cursor link that dispatches through the registered
 * Platform_OpenInShellFn. Non-URL text wraps via TextWrapped. Explicit '\n'
 * line breaks in the source string are preserved. URL detection: a run
 * starting with "http://" or "https://" at the start of a line or after
 * whitespace/bracket, ending at the next whitespace, then trim trailing
 * .,;:!?)] '" so "see https://x.com." doesn't pull the period into the link.
 *
 * Caveat: lines that contain a URL render without mid-line wrapping (the
 * link itself is atomic, and prefix/suffix segments emit via TextUnformatted
 * inside a push/pop wrap pair). Fine for the short tutorial/about lines that
 * use this today; if a longer URL surfaces, cap dialog width or pre-wrap. */
static inline void imguiTextWrappedWithLinks(const char *text) {
    if (!text) text = "";

    const char *lineStart = text;
    while (*lineStart) {
        const char *lineEnd = lineStart;
        while (*lineEnd && *lineEnd != '\n') lineEnd++;

        /* Scan the line for http:// or https:// at a valid boundary */
        const char *urlStart = nullptr;
        for (const char *scan = lineStart; scan < lineEnd; scan++) {
            const size_t remain = (size_t)(lineEnd - scan);
            bool isHttp  = (remain >= 7 && memcmp(scan, "http://",  7) == 0);
            bool isHttps = (remain >= 8 && memcmp(scan, "https://", 8) == 0);
            if (!isHttp && !isHttps) continue;
            if (scan == lineStart ||
                (unsigned char)scan[-1] <= ' ' ||
                scan[-1] == '(' || scan[-1] == '[' || scan[-1] == '<') {
                urlStart = scan;
                break;
            }
        }

        if (!urlStart) {
            ImGui::TextWrapped("%.*s", (int)(lineEnd - lineStart), lineStart);
        } else {
            const char *urlEnd = urlStart;
            while (urlEnd < lineEnd && (unsigned char)*urlEnd > ' ') urlEnd++;
            while (urlEnd > urlStart) {
                char c = urlEnd[-1];
                if (c == '.' || c == ',' || c == ';' || c == ':' || c == '!' ||
                    c == '?' || c == ')' || c == ']' || c == '\'' || c == '"') {
                    urlEnd--;
                } else {
                    break;
                }
            }

            ImGui::PushTextWrapPos(0.0f);
            if (urlStart > lineStart) {
                ImGui::TextUnformatted(lineStart, urlStart);
                ImGui::SameLine(0, 0);
            }

            char urlBuf[512];
            size_t urlLen = (size_t)(urlEnd - urlStart);
            if (urlLen >= sizeof(urlBuf)) urlLen = sizeof(urlBuf) - 1;
            memcpy(urlBuf, urlStart, urlLen);
            urlBuf[urlLen] = '\0';
            ImGui::TextLinkOpenURL(urlBuf);

            if (urlEnd < lineEnd) {
                ImGui::SameLine(0, 0);
                ImGui::TextUnformatted(urlEnd, lineEnd);
            }
            ImGui::PopTextWrapPos();
        }

        lineStart = lineEnd;
        if (*lineStart == '\n') lineStart++;
    }
}

/* Override DisplayFramebufferScale after ImGui_ImplSDL3_NewFrame().
 * When a logical presentation is active, SDL already maps point-space
 * coordinates to native pixels.  ImGui_ImplSDL3_NewFrame() detects the
 * Retina scale and sets DisplayFramebufferScale to e.g. 3.0, which
 * causes the ImGui renderer to double-scale vertices.  Reset to 1.0
 * so the logical presentation is the only scaling layer. */
static inline void dialogOverrideFramebufferScale(SDL_Renderer *renderer) {
    SDL_Window *win = SDL_GetRenderWindow(renderer);
    if (win && (SDL_GetWindowFlags(win) & SDL_WINDOW_HIGH_PIXEL_DENSITY)) {
        ImGuiIO &io = ImGui::GetIO();
        io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
    }
}

/* Set up a scaled font and touch-friendly ImGui style.
 * Call after ImGui::CreateContext() and before the first NewFrame().
 * Always loads the TTF font; on mobile also applies touch-friendly styling. */
static inline void dialogApplyScaling(float uiScale) {
    if (uiScale <= 1.05f) {
        imguiLoadBoloFont(18.0f);
        return;
    }

    imguiLoadBoloFont(20.0f * uiScale);

    ImGuiStyle &style = ImGui::GetStyle();
    style.ScaleAllSizes(uiScale);

    /* Touch-friendly padding for mobile (uiScale > 1 because of physical
     * screen size).  Skipped on Deck: it routes here too (uiScale ==
     * deckMul ≥ 2 from dialogComputeScale) but uses desktop hover/click
     * feel, not touch. */
    if (!uiModeIsSteamDeck()) {
        style.TouchExtraPadding = ImVec2(8.0f, 8.0f);
    }
}

/* Save logical presentation before a dialog (Android needs this).
 * When HIGH_PIXEL_DENSITY is active, we set a point-space logical
 * presentation so dialogs (which use SDL_GetWindowSize for coordinates)
 * render correctly onto the native-resolution backing buffer. */
static inline void dialogSaveLogicalPresentation(SDL_Renderer *renderer,
                                                  int *outW, int *outH,
                                                  SDL_RendererLogicalPresentation *outMode) {
    SDL_GetRenderLogicalPresentation(renderer, outW, outH, outMode);
    SDL_Window *win = SDL_GetRenderWindow(renderer);
    if (win && (SDL_GetWindowFlags(win) & SDL_WINDOW_HIGH_PIXEL_DENSITY)) {
        int winW = 0, winH = 0;
        SDL_GetWindowSize(win, &winW, &winH);
        if (winW > 0 && winH > 0) {
            SDL_SetRenderLogicalPresentation(renderer, winW, winH,
                                             SDL_LOGICAL_PRESENTATION_LETTERBOX);
        }
    } else {
        SDL_SetRenderLogicalPresentation(renderer, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED);
    }
}

/* Restore logical presentation after a dialog. */
static inline void dialogRestoreLogicalPresentation(SDL_Renderer *renderer,
                                                     int w, int h,
                                                     SDL_RendererLogicalPresentation mode) {
    SDL_SetRenderLogicalPresentation(renderer, w, h, mode);
}

/* Query safe area insets for notch/Dynamic Island avoidance.
 * Returns insets in window-point coordinates (left, top, right, bottom).
 * On platforms without safe areas, all values are 0. */
struct DialogSafeInsets {
    float left, top, right, bottom;
};

static inline DialogSafeInsets dialogGetSafeInsets(SDL_Window *window) {
    DialogSafeInsets insets = {0, 0, 0, 0};
#if BOLO_MOBILE
    SDL_Rect safeRect;
    int winW = 0, winH = 0;
    SDL_GetWindowSize(window, &winW, &winH);
    if (winW > 0 && winH > 0 && SDL_GetWindowSafeArea(window, &safeRect)) {
        insets.left   = (float)safeRect.x;
        insets.top    = (float)safeRect.y;
        insets.right  = (float)(winW - (safeRect.x + safeRect.w));
        insets.bottom = (float)(winH - (safeRect.y + safeRect.h));
    }
#else
    (void)window;
#endif
    return insets;
}

/* Frame rate cap for mobile platforms (Android/iOS).
 * Call at the top of the render loop to record frame start, then call
 * dialogFrameCapEnd() after SDL_RenderPresent to delay if needed.
 * Targets 60 FPS (16ms per frame). */
#if defined(__ANDROID__) || defined(__IPHONEOS__)
#define DIALOG_FRAME_CAP_MS 16

static inline Uint64 dialogFrameCapBegin(void) {
    return SDL_GetTicks();
}

static inline void dialogFrameCapEnd(Uint64 frameStart) {
    Uint64 elapsed = SDL_GetTicks() - frameStart;
    if (elapsed < DIALOG_FRAME_CAP_MS) {
        SDL_Delay((Uint32)(DIALOG_FRAME_CAP_MS - elapsed));
    }
}
#elif defined(__EMSCRIPTEN__)
/* Browser: wait for the next animation frame (main_wasm.c), which paces the
 * dialog loops and yields to the browser between frames. */
#ifdef __cplusplus
extern "C"
#endif
void wasmFrameWait(void);

static inline Uint64 dialogFrameCapBegin(void) { return 0; }
static inline void dialogFrameCapEnd(Uint64 frameStart) {
    (void)frameStart;
    wasmFrameWait();
}
#else
static inline Uint64 dialogFrameCapBegin(void) { return 0; }
static inline void dialogFrameCapEnd(Uint64 frameStart) { (void)frameStart; }
#endif

/* -------------------------------------------------------
 * Device resolution presets for Ctrl+T cycling.
 * Shared between the main game loop and dialog event loops.
 * ------------------------------------------------------- */

#ifdef __cplusplus
extern "C" {
#endif
#include "../../ui_mode.h"
#ifdef __cplusplus
}
#endif

struct DevicePreset {
  const char *name;
  int w, h;
  UIMode mode;
};

static const DevicePreset s_devicePresets[] = {
  /* Phones (landscape) */
  { "iPhone SE",            1334,  750,  UI_MODE_TABLET  },
  { "iPhone 15",            2556, 1179,  UI_MODE_TABLET  },
  { "iPhone 15 Pro Max",    2796, 1290,  UI_MODE_TABLET  },
  { "Samsung Galaxy S24",   2340, 1080,  UI_MODE_TABLET  },
  { "Google Pixel 8",       2400, 1080,  UI_MODE_TABLET  },
  /* Tablets (landscape) */
  { "iPad Mini",            2266, 1488,  UI_MODE_TABLET  },
  { "iPad Air",             2360, 1640,  UI_MODE_TABLET  },
  { "iPad Pro 11\"",        2388, 1668,  UI_MODE_TABLET  },
  { "iPad Pro 12.9\"",      2732, 2048,  UI_MODE_TABLET  },
  { "Samsung Galaxy Tab S9", 2560, 1600, UI_MODE_TABLET  },
  /* Handheld (landscape) */
  { "Steam Deck",           1280,  800,  UI_MODE_STEAM_DECK },
  /* Back to desktop */
  { "Desktop",                 0,    0,  UI_MODE_DESKTOP },
};
static const int s_numDevicePresets = (int)(sizeof(s_devicePresets) / sizeof(s_devicePresets[0]));
/* Defined in ui_mode.c to share across translation units */
#ifdef __cplusplus
extern "C" {
#endif
extern int g_currentDevicePreset;
#ifdef __cplusplus
}
#endif

/* Set dialog window size; only re-center if the size actually changed.
 * If a device preset is active, uses the preset dimensions instead.
 * In controller mode (Deck always, desktop when the player opted in via
 * Controller Mode), leave the host window alone so dialogs render into
 * the existing fullscreen surface — clamping to a 1024x768 default
 * would clip below smaller-than-1024 screen heights, and forcing a
 * resize while the player is using a controller is jarring.
 * A fullscreen window is left alone for the same reason: its size is the
 * display's, so resizing or recentring it either does nothing or fights
 * the compositor. */
static inline void dialogSetWindowSize(SDL_Window *window, int w, int h) {
    /* The page sizes a page-filling canvas, not the dialog. */
    if (SDL_GetWindowFlags(window) & SDL_WINDOW_FILL_DOCUMENT) {
        return;
    }
    if (uiShouldUseControllerMode()) {
        return;
    }
    if (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) {
        return;
    }
    if (g_currentDevicePreset >= 0 && g_currentDevicePreset < s_numDevicePresets &&
        s_devicePresets[g_currentDevicePreset].mode != UI_MODE_DESKTOP) {
        w = s_devicePresets[g_currentDevicePreset].w;
        h = s_devicePresets[g_currentDevicePreset].h;
    }
    int curW = 0, curH = 0;
    SDL_GetWindowSize(window, &curW, &curH);
    SDL_SetWindowSize(window, w, h);
    if (curW != w || curH != h) {
        SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
}

/* Set dialog window title; appends device preset info when one is active. */
static inline void dialogSetWindowTitle(SDL_Window *window, const char *title) {
    /* On a page-filling canvas the page's shell owns the tab title. */
    if (SDL_GetWindowFlags(window) & SDL_WINDOW_FILL_DOCUMENT) {
        return;
    }
    if (g_currentDevicePreset >= 0 && g_currentDevicePreset < s_numDevicePresets &&
        s_devicePresets[g_currentDevicePreset].mode != UI_MODE_DESKTOP) {
        const DevicePreset *p = &s_devicePresets[g_currentDevicePreset];
        char buf[192];
        SDL_snprintf(buf, sizeof(buf), "%s - %s (%dx%d)", title, p->name, p->w, p->h);
        SDL_SetWindowTitle(window, buf);
    } else {
        SDL_SetWindowTitle(window, title);
    }
}

/* App full screen mode — the window state every screen runs at.
 * Stored in gamefront.c, loaded/saved via INI prefs. */
#ifdef __cplusplus
extern "C" {
#endif
extern bool gameFrontFullScreen;
#ifdef __cplusplus
}
#endif

/* Apply a device preset to the given window. Updates UI mode and window size/title. */
static inline void dialogApplyDevicePreset(SDL_Window *win, int idx) {
    g_currentDevicePreset = idx;
    const DevicePreset *p = &s_devicePresets[idx];
    if (!win) return;

    uiModeSet(p->mode);

    if (p->mode == UI_MODE_DESKTOP) {
        SDL_SetWindowFullscreen(win, false);
        SDL_SetWindowSize(win, 1024, 768);
        SDL_SetWindowTitle(win, "WinBolo");
        /* The sizing above needs a windowed window, so full screen goes back
           on afterwards and the flag and the window still agree. */
        SDL_SetWindowFullscreen(win, gameFrontFullScreen);
    } else {
        SDL_SetWindowFullscreen(win, false);
        SDL_SetWindowSize(win, p->w, p->h);
        char title[128];
        SDL_snprintf(title, sizeof(title), "WinBolo - %s (%dx%d)", p->name, p->w, p->h);
        SDL_SetWindowTitle(win, title);
        /* A simulated device runs at its own size, so the window stays
           windowed and the flag follows it. */
        gameFrontFullScreen = false;
    }

    WB_LOG_INFO(WB_LOG_CAT_GUI, "Device preset: %s (%dx%d, %s)", p->name, p->w, p->h,
            p->mode == UI_MODE_STEAM_DECK ? "STEAM_DECK" :
            p->mode == UI_MODE_TABLET ? "TABLET" : "DESKTOP");
}

static inline bool dialogHandleDevicePresetEvent(SDL_Window *win, const SDL_Event *ev) {
    (void)win;
    (void)ev;
    return false;
}

/* Alt+Enter is the full screen key across the app: in a game sdl3imgui.cpp
 * reads it off the event, and the screens outside one read it here. A
 * predicate rather than an action, because what a screen has to redo for the
 * surface it lands on differs -- the welcome screen comes back in through its
 * own top, the lobby rebuilds its chrome in place.
 * Test it before ImGui_ImplSDL3_ProcessEvent and consume the event: Enter is
 * live in a focused text field (the lobby's chat line sends on it), and this
 * keystroke is a window command rather than typing.
 * Deck and tablet are full screen from window creation and keep no windowed
 * geometry to come back to, so there it is not a toggle at all. */
static inline bool dialogIsFullScreenToggleEvent(SDL_Window *win, const SDL_Event *ev) {
#if BOLO_MOBILE || defined(__EMSCRIPTEN__)
    (void)win;
    (void)ev;
    return false;
#else
    if (!win || !ev) return false;
    if (uiModeIsSteamDeck() || uiModeIsTablet()) return false;
    return ev->type == SDL_EVENT_KEY_DOWN && !ev->key.repeat &&
           ev->key.windowID == SDL_GetWindowID(win) &&
           (ev->key.mod & SDL_KMOD_ALT) != 0 &&
           (ev->key.scancode == SDL_SCANCODE_RETURN ||
            ev->key.scancode == SDL_SCANCODE_KP_ENTER);
#endif
}

/* Dialog window position — separate from game window position.
 * Stored in gamefront.c, loaded/saved via INI prefs. */
#ifdef __cplusplus
extern "C" {
#endif
extern int gameFrontDialogX;
extern int gameFrontDialogY;
#ifdef __cplusplus
}
#endif

/* Save current dialog window position. Skipped while the window is
 * fullscreen: that position is wherever the display put it, not the
 * windowed position the player chose, and saving it would overwrite the
 * one they get back when full screen goes off. */
static inline void dialogSaveCurrentPosition(SDL_Window *win) {
    if (win && !(SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN)) {
        SDL_GetWindowPosition(win, &gameFrontDialogX, &gameFrontDialogY);
    }
}

/* Handle move/resize events — save dialog position */
static inline void dialogHandleWindowMoveResize(SDL_Window *win, const SDL_Event *ev) {
    if (!win) return;
    SDL_WindowID winID = SDL_GetWindowID(win);
    if ((ev->type == SDL_EVENT_WINDOW_MOVED || ev->type == SDL_EVENT_WINDOW_RESIZED) &&
        ev->window.windowID == winID) {
        dialogSaveCurrentPosition(win);
    }
}

/* Restore dialog window position if we have a saved one. Skipped while the
 * window is fullscreen: it already covers the display, so moving it either
 * does nothing or fights the compositor. */
static inline void dialogRestorePosition(SDL_Window *win) {
    /* A page-filling canvas sits where the page puts it. */
    if (win && !(SDL_GetWindowFlags(win) &
                 (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FILL_DOCUMENT)) &&
        gameFrontDialogX >= 0 && gameFrontDialogY >= 0) {
        SDL_SetWindowPosition(win, gameFrontDialogX, gameFrontDialogY);
    }
}

/* Convert a gamepad B-button (EAST) press into a window-close request
 * for the given dialog window.  Lets controller users cancel any
 * standalone dialog with B, routing through the dialog's existing
 * SDL_EVENT_WINDOW_CLOSE_REQUESTED handler so behaviour matches the X
 * close button.  Skipped while:
 *   - an ImGui popup is open (B closes the popup)
 *   - text input is active (B clears the field)
 *   - nav focus is inside a child window or table scroll region
 *     (B pops out of the sub-region via ImGui's NavCancel)
 * Call AFTER ImGui_ImplSDL3_ProcessEvent so ImGui still sees the raw
 * gamepad event (it polls button state, not the event, but be safe). */
extern "C" bool dialogNavIsInsideSubRegion(void);  /* imgui_nav_outline.cpp */
extern "C" bool dialogNavWasInsideSubRegionAtFrameStart(void);  /* imgui_nav_outline.cpp */
static inline bool dialogHandleGamepadCancelEvent(SDL_Window *window, SDL_Event *ev) {
    if (!ev || !window) return false;
    if (ev->type != SDL_EVENT_GAMEPAD_BUTTON_DOWN) return false;
    if (ev->gbutton.button != SDL_GAMEPAD_BUTTON_EAST) return false;
    if (ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup)) return false;
    if (ImGui::GetIO().WantTextInput) return false;
    if (dialogNavIsInsideSubRegion()) return false;
    SDL_zero(*ev);
    ev->type = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
    ev->window.windowID = SDL_GetWindowID(window);
    ev->window.timestamp = SDL_GetTicksNS();
    ev->window.data1 = DIALOG_CLOSE_IS_GAMEPAD_CANCEL;
    return true;
}

/* Every screen runs its own event loop, so a quit has to be recognised in
 * each of them or it stops at whichever one is on top — which is what used
 * to happen: Cmd+Q in the game browser closed the browser and left the
 * player on the menu.  Call this from a dialog's poll loop and end the loop
 * when it returns true.
 *
 * Cmd+Q (SDL_EVENT_QUIT), Alt+F4 and the window's close box all mean the
 * application should end, so they are handed to the host's quit handler and
 * the front end shuts down instead of dropping back a screen.  A B press is
 * a cancel and is left alone: it arrives as the close request
 * dialogHandleGamepadCancelEvent forged, marked as its own. */
static inline bool dialogHandleQuitEvent(SDL_Window *window, const SDL_Event *ev) {
    const DialogQuitAction action =
        dialogQuitClassify(ev, window ? SDL_GetWindowID(window) : 0);
    if (action == DIALOG_QUIT_APPLICATION) dialogRequestQuit();
    return action != DIALOG_QUIT_NONE;
}

/* Check an SDL event for a winbolo:// URL drop.
 * Returns true if the event was consumed and the caller's dialog
 * should exit (dlgState has been changed). */
extern "C" void gameFrontHandleUrlOpen(char *url);  /* from gamefront.h */
static inline bool dialogHandleUrlDropEvent(const SDL_Event *ev) {
    if (ev->type == SDL_EVENT_DROP_FILE && ev->drop.data) {
        const char *url = ev->drop.data;
        if (SDL_strncmp(url, "winbolo://", 10) == 0) {
            char urlCopy[512];
            SDL_strlcpy(urlCopy, url, sizeof(urlCopy));
            gameFrontHandleUrlOpen(urlCopy);
            WB_LOG_INFO(WB_LOG_CAT_GUI, "[URL] Received winbolo:// link on menu: %s", url);
            return true;
        }
    }
    return false;
}

/* ---------------------------------------------------------------
 * Standard popup modal fade-in.
 *
 * BeginPopupModal otherwise snaps in at full opacity, which feels
 * abrupt next to the welcome screen's own ~150ms alpha ramp.  Call
 * this *inside* the popup body and push the returned value as
 * ImGuiStyleVar_Alpha; pop and EndPopup before exiting the body.
 * Push and pop must live in the popup window's own scope so the
 * style-stack stays balanced per-window.
 *
 * Phase resets to 0 on the first frame the popup window appears
 * (detected via IsWindowAppearing, which is window-scoped and works
 * correctly from inside the body — unlike IsPopupOpen, which is
 * scoped to the parent ID stack and would always read false here).
 *
 * Fade-out isn't supported (BeginPopupModal returns false on the
 * frame after CloseCurrentPopup, leaving nowhere to draw a ramp-out),
 * so dismissal stays one-frame snappy.  The body's fade-in covers
 * the perceived "popping in"; the modal dim-bg remains at full
 * opacity since it is drawn before the body runs, which is
 * acceptable.  Usage:
 *
 *     static float s_fade = 0.0f;
 *     if (ImGui::BeginPopupModal(popupId, ...)) {
 *         ImGui::PushStyleVar(ImGuiStyleVar_Alpha,
 *                             imguiPopupFadeAlpha(&s_fade));
 *         ... body ...
 *         ImGui::PopStyleVar();
 *         ImGui::EndPopup();
 *     }
 */
#ifdef __cplusplus
static inline float imguiPopupFadeAlpha(float *phase,
                                        float fadeInSec = 0.15f) {
    if (ImGui::IsWindowAppearing()) {
        *phase = 0.0f;
    }
    const float step = ImGui::GetIO().DeltaTime / fadeInSec;
    *phase = (*phase + step >= 1.0f) ? 1.0f : *phase + step;
    return *phase;
}
#endif /* __cplusplus */

#endif /* IMGUI_DIALOG_UTILS_H */
