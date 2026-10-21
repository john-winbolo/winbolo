/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
*Name:          Android Device
*Filename:      android_device.h
*Purpose:
*  What the device is, read from WinBoloActivity over JNI:
*  whether its screen is a tablet's (the configuration's
*  smallest width, 600 dp and over), which ui_mode.c turns
*  into the classic screen with the touch controls, the
*  height of the on-screen keyboard while it is up, which
*  the dialogs use to keep a text field above it, and the
*  safe area's insets (the display cutout and the
*  navigation bar), which safe_insets.h hands to every
*  mobile screen.
*********************************************************/

#ifndef ANDROID_DEVICE_H
#define ANDROID_DEVICE_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The directory main_android.c extracts the asset trees into, under the
   internal storage directory, and makes the working directory. */
#define ANDROID_APP_DIR "app"

/*********************************************************
*NAME:          androidDeviceAppPath
*PURPOSE:
*  The absolute path of a shipped file: rel, a path under
*  the data or brain tree such as data/fonts/X.ttf, under
*  ANDROID_APP_DIR. For opens that go through SDL
*  (SDL_IOFromFile, so TTF_OpenFont and the rest), which
*  resolves a relative path against the internal storage
*  root, not the working directory, and so misses the
*  copy and reads the APK's asset instead; that asset is
*  deflated, and a reader that seeks, as FreeType does,
*  inflates it again at every seek. Returns false when
*  there is no internal storage path or the result does
*  not fit.
*********************************************************/
bool androidDeviceAppPath(const char *rel, char *out, size_t cap);

/*********************************************************
*NAME:          androidDeviceInit
*PURPOSE:
*  Finds WinBoloActivity from the main thread (whose class
*  loader can see it) and reads the smallest width once.
*  Call before anything asks the UI mode. Returns false
*  when the class is not found, in which case the device
*  is a phone.
*********************************************************/
bool androidDeviceInit(void);

/*********************************************************
*NAME:          androidDeviceIsTablet
*PURPOSE:
*  True when the configuration's smallest width is 600 dp
*  or more. Declared for the shared code in ui_mode.h.
*********************************************************/
bool androidDeviceIsTablet(void);

/*********************************************************
*NAME:          androidDeviceKeyboardHeight
*PURPOSE:
*  The on-screen keyboard's height in window pixels while
*  it is up, 0 while it is down, and -1 where Android does
*  not report it (before Android 11). Declared for the
*  shared code in dialogs/imgui_touch.h.
*********************************************************/
int androidDeviceKeyboardHeight(void);

/*********************************************************
*NAME:          androidDeviceShowKeyboard
*PURPOSE:
*  Shows the keyboard again for the text field being
*  typed in after the Back key hid it, through the input
*  method directly (WinBoloActivity.showKeyboardAgain).
*  Declared for the shared code in dialogs/imgui_touch.h.
*********************************************************/
void androidDeviceShowKeyboard(void);

/*********************************************************
*NAME:          androidDeviceSafeInsets
*PURPOSE:
*  The safe area's insets in window pixels, left, top,
*  right, bottom: the display cutout and the navigation
*  bar, as WinBoloActivity.safeInsetsPx reads them from
*  the window's insets, and nothing else; SDL's own safe
*  area adds the system gesture zones, a 48 dp band along
*  the top of every landscape screen. Returns false, the
*  insets untouched, until the window's insets have
*  arrived. Declared for the shared code in
*  gui/sdl3/safe_insets.h.
*********************************************************/
bool androidDeviceSafeInsets(int *left, int *top, int *right, int *bottom);

#ifdef __cplusplus
}
#endif

#endif /* ANDROID_DEVICE_H */
