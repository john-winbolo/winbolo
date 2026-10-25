/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
*Name:          Android Share
*Filename:      android_share.h
*Purpose:
*  The share sheet: a file from the app's files or cache
*  directory offered to another app, which is how a
*  recording or a clip GIF leaves the phone. The sheet is
*  WinBoloActivity.shareFile, an ACTION_SEND chooser on a
*  content URI the app's FileProvider serves; the call here
*  reaches it through JNI.
*********************************************************/

#ifndef ANDROID_SHARE_H
#define ANDROID_SHARE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*********************************************************
*NAME:          androidShareInstall
*PURPOSE:
*  Finds the activity class and its shareFile method, from
*  the thread Java created, whose class loader can see it.
*  Called once from main before the first screen. Returns
*  false when the method is not there; androidShareFile
*  then answers false.
*
*ARGUMENTS:
*  (none)
*********************************************************/
bool androidShareInstall(void);

/*********************************************************
*NAME:          androidShareFile
*PURPOSE:
*  Shows the share sheet for the file at path, which must be
*  under the app's files or cache directory (the two the
*  FileProvider serves). Returns when the chooser has been
*  started, not when the player has picked; false when it
*  could not be.
*
*ARGUMENTS:
*  path - the file's full path
*  mime - its media type ("application/octet-stream",
*         "image/gif")
*********************************************************/
bool androidShareFile(const char *path, const char *mime);

#ifdef __cplusplus
}
#endif

#endif /* ANDROID_SHARE_H */
