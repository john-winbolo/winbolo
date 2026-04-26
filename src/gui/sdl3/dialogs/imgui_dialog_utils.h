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
#include "../../imgui_fonts.h"
#include "nanosvg.h"
#include "nanosvgrast.h"

/* Unified mobile platform check — use BOLO_MOBILE instead of
 * repeating __ANDROID__ || __IPHONEOS__ everywhere. */
#if defined(__ANDROID__) || defined(__IPHONEOS__)
#define BOLO_MOBILE 1
#else
#define BOLO_MOBILE 0
#endif

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
 * On desktop we control the dialog window size, so scale is always 1.0.
 * On Android (and similar full-screen platforms) the dialog renders into
 * the device's full window, so we scale based on screen height.
 * Reference height is 540px (1x scale). */
static inline float dialogComputeScale(int screenW, int screenH) {
    (void)screenW;
#if BOLO_MOBILE
    float scale = (float)screenH / 540.0f;
    if (scale < 1.0f) scale = 1.0f;
    return scale;
#else
    (void)screenH;
    return 1.0f;
#endif
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

    /* Apply touch-friendly padding on high-scale displays (mobile, Steam Deck) */
    ImGuiStyle &style = ImGui::GetStyle();
    style.TouchExtraPadding = ImVec2(8.0f, 8.0f);
    style.ScaleAllSizes(uiScale);
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
  { "Steam Deck",           1280,  800,  UI_MODE_TABLET  },
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
 * If a device preset is active, uses the preset dimensions instead. */
static inline void dialogSetWindowSize(SDL_Window *window, int w, int h) {
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
    } else {
        SDL_SetWindowFullscreen(win, false);
        SDL_SetWindowSize(win, p->w, p->h);
        char title[128];
        SDL_snprintf(title, sizeof(title), "WinBolo - %s (%dx%d)", p->name, p->w, p->h);
        SDL_SetWindowTitle(win, title);
    }

    SDL_Log("Device preset: %s (%dx%d, %s)", p->name, p->w, p->h,
            p->mode == UI_MODE_TABLET ? "TABLET" : "DESKTOP");
}

/* Cycle to the next device preset. Call from Ctrl+T handler. */
static inline void dialogCycleDevicePreset(SDL_Window *win) {
    g_currentDevicePreset = (g_currentDevicePreset + 1) % s_numDevicePresets;
    dialogApplyDevicePreset(win, g_currentDevicePreset);
}

/* Check an SDL event for Ctrl+T and cycle presets if matched.
 * Returns true if the event was consumed. */
static inline bool dialogHandleDevicePresetEvent(SDL_Window *win, const SDL_Event *ev) {
    if (ev->type == SDL_EVENT_KEY_DOWN &&
        (ev->key.mod & SDL_KMOD_CTRL) &&
        ev->key.scancode == SDL_SCANCODE_T) {
        dialogCycleDevicePreset(win);
        return true;
    }
    return false;
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

/* Save current dialog window position */
static inline void dialogSaveCurrentPosition(SDL_Window *win) {
    if (win) {
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

/* Restore dialog window position if we have a saved one */
static inline void dialogRestorePosition(SDL_Window *win) {
    if (win && gameFrontDialogX >= 0 && gameFrontDialogY >= 0) {
        SDL_SetWindowPosition(win, gameFrontDialogX, gameFrontDialogY);
    }
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
            SDL_Log("[URL] Received winbolo:// link on menu: %s", url);
            return true;
        }
    }
    return false;
}

#endif /* IMGUI_DIALOG_UTILS_H */
