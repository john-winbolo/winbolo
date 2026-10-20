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
*  into the classic screen with the touch controls, and
*  the height of the on-screen keyboard while it is up,
*  which the dialogs use to keep a text field above it.
*********************************************************/

#ifndef ANDROID_DEVICE_H
#define ANDROID_DEVICE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

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

#ifdef __cplusplus
}
#endif

#endif /* ANDROID_DEVICE_H */
