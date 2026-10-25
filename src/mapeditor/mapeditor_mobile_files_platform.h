/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 * Name:          mapeditor_mobile_files_platform.h
 * Purpose:       The platform half of the map editor's
 *                file picker (mapeditor_mobile_files.cpp):
 *                which two directories it lists. iOS answers
 *                from mapeditor_ios_files.cpp (the app's
 *                Documents folder, which the Files app shows
 *                under WinBolo, and the maps in the app
 *                bundle). Android answers from
 *                src/android/android_mapeditor_files.c (the
 *                Maps folder under SDL's preferences path,
 *                where the in-game Save Map writes, and the
 *                maps extracted from the APK under the
 *                working directory).
 *********************************************************/

#ifndef MAPEDITOR_MOBILE_FILES_PLATFORM_H
#define MAPEDITOR_MOBILE_FILES_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The folder the player's own files live in, where saves go, without a
 * trailing slash. The platform creates it when it does not exist yet. False
 * when SDL cannot say where it is. */
bool meMobileFilesPlatformUserDir(char *out, size_t cap);

/* The folder of the maps shipped with the app, read-only, without a trailing
 * slash. False when the platform has none to list. */
bool meMobileFilesPlatformBundledDir(char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* MAPEDITOR_MOBILE_FILES_PLATFORM_H */
