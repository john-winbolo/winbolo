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
 *  though the cutout is on one side only; an Android
 *  phone with gesture navigation reports equal side
 *  insets too. Here the side away from the cutout gets
 *  SAFE_INSETS_EDGE_MARGIN_FRAC of the window's width
 *  instead, which keeps clear of the screen's rounded
 *  corners. The cutout's side is found from the display
 *  orientation. Unequal insets are left as they are. Zero
 *  everywhere off mobile.
 *********************************************************/

#ifndef SAFE_INSETS_H
#define SAFE_INSETS_H

#include <SDL3/SDL.h>

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
    SDL_GetWindowSize(window, &winW, &winH);
    if (winW > 0 && winH > 0 && SDL_GetWindowSafeArea(window, &safeRect)) {
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
