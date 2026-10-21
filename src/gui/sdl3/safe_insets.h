/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 * Name:          safe_insets.h
 * Purpose:       The screen's safe insets as every mobile
 *                screen uses them, for C and C++ alike: the
 *                dialogs (dialogGetSafeInsets in
 *                imgui_dialog_utils.h) and the touch layout's
 *                chrome in sdl3draw.c. Header-only, so the
 *                standalone tools that include the dialog
 *                header link nothing more.
 *
 *  In landscape a phone with a notch or Dynamic Island
 *  reports the same inset on the left and the right,
 *  though the cutout is on one side only. Here the side
 *  away from the cutout gets SAFE_INSETS_EDGE_MARGIN_FRAC
 *  of the window's width instead, which keeps clear of
 *  the screen's rounded corners. The cutout's side is
 *  found from the display orientation. Unequal insets are
 *  left as they are. Zero everywhere off mobile.
 *
 *  On Android the insets are the display cutout and the
 *  navigation bar (androidDeviceSafeInsets), what iOS's
 *  safe area carries; SDL_GetWindowSafeArea folds in the
 *  system gesture zones as well, a 48 dp band along the
 *  top of every landscape screen, and is read only until
 *  the window's insets have reached the Java side. A phone
 *  with no cutout then reports equal side insets of zero,
 *  which the rule above leaves alone.
 *********************************************************/

#ifndef SAFE_INSETS_H
#define SAFE_INSETS_H

#include <SDL3/SDL.h>

#if defined(__ANDROID__)
#include "../../android/android_device.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* The share of the window's width or height a mobile screen keeps from an
 * edge with no cutout: the log browser's margin. */
#define SAFE_INSETS_EDGE_MARGIN_FRAC 0.025f

typedef struct {
    float left, top, right, bottom;   /* window points */
} SafeInsets;

static inline SafeInsets safeInsetsGet(SDL_Window *window) {
    SafeInsets insets = {0.0f, 0.0f, 0.0f, 0.0f};
#if defined(__ANDROID__) || defined(__IPHONEOS__)
    SDL_Rect safeRect;
    int winW = 0, winH = 0;
    bool haveInsets = false;
    SDL_GetWindowSize(window, &winW, &winH);
#if defined(__ANDROID__)
    {
        /* The cutout and the navigation bar, in window pixels (an Android
           window's points are its pixels). */
        int l = 0, t = 0, r = 0, b = 0;
        if (androidDeviceSafeInsets(&l, &t, &r, &b)) {
            insets.left   = (float)l;
            insets.top    = (float)t;
            insets.right  = (float)r;
            insets.bottom = (float)b;
            haveInsets = true;
        }
    }
#endif
    if (!haveInsets && winW > 0 && winH > 0 && SDL_GetWindowSafeArea(window, &safeRect)) {
        insets.left   = (float)safeRect.x;
        insets.top    = (float)safeRect.y;
        insets.right  = (float)(winW - (safeRect.x + safeRect.w));
        insets.bottom = (float)(winH - (safeRect.y + safeRect.h));
    }
    {
        const float farMargin = (float)winW * SAFE_INSETS_EDGE_MARGIN_FRAC;
        if (insets.left > farMargin && insets.left == insets.right) {
            switch (SDL_GetCurrentDisplayOrientation(SDL_GetDisplayForWindow(window))) {
            case SDL_ORIENTATION_LANDSCAPE:          /* cutout on the left */
                insets.right = farMargin;
                break;
            case SDL_ORIENTATION_LANDSCAPE_FLIPPED:  /* cutout on the right */
                insets.left = farMargin;
                break;
            default:
                break;
            }
        }
    }
#else
    (void)window;
#endif
    return insets;
}

#ifdef __cplusplus
}
#endif

#endif /* SAFE_INSETS_H */
