/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
*Name:          Android Drops
*Filename:      android_drops.h
*Purpose:
*  What the system hands the Android app to open - a .wbv
*  recording from Files, Mail or another app, or a
*  winbolo:// game link - kept until the welcome screen can
*  act on it. The Android counterpart of ios_drops.h.
*
*  A cold launch arrives as main()'s argument (see
*  WinBoloActivity.getArguments). While the app runs,
*  WinBoloActivity.onNewIntent hands the link or the
*  recording's path to the two natives this file defines,
*  which keep it here; the welcome screen takes it the
*  next time it runs, so one arriving during a game waits
*  for the game to end. The in-game loop is never told.
*********************************************************/

#ifndef ANDROID_DROPS_H
#define ANDROID_DROPS_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*********************************************************
*NAME:          androidDropsInstall
*PURPOSE:
*  Creates the lock the kept items are read and written
*  under. Called once from main before the first screen.
*
*ARGUMENTS:
*  (none)
*********************************************************/
void androidDropsInstall(void);

/*********************************************************
*NAME:          androidDropsKeepRecording
*PURPOSE:
*  Keeps a recording's path for the welcome screen, as the
*  native from onNewIntent does. For main, which gets a
*  cold launch's recording as its argument.
*
*ARGUMENTS:
*  path - the recording's path
*********************************************************/
void androidDropsKeepRecording(const char *path);

/*********************************************************
*NAME:          androidDropsForgetLink
*PURPOSE:
*  Drops the kept link when it is the one given: the game
*  has handled it itself (a tablet's classic screen takes
*  the desktop's event handling, which sees the link in a
*  game), so the welcome screen does not join it again.
*
*ARGUMENTS:
*  link - the winbolo:// link the game handled
*********************************************************/
void androidDropsForgetLink(const char *link);

/*********************************************************
*NAME:          androidDropsWelcomeResult
*PURPOSE:
*  What the welcome screen should open for a kept item, or
*  -1 for none. A game link is handed to
*  gameFrontHandleUrlOpen here and answers
*  openInternetManual, as the desktop's welcome loop does
*  for one it polls. A recording answers openLogViewer and
*  stays kept for the Log Viewer entry in gamefront.c to
*  take and play.
*
*ARGUMENTS:
*  (none)
*********************************************************/
int androidDropsWelcomeResult(void);

/*********************************************************
*NAME:          androidDropsTakeLog
*PURPOSE:
*  Takes the kept recording's path, if there is one.
*
*ARGUMENTS:
*  out  - receives the path
*  size - size of out
*********************************************************/
bool androidDropsTakeLog(char *out, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* ANDROID_DROPS_H */
