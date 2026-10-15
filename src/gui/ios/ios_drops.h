/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
*Name:          iOS Drops
*Filename:      ios_drops.h
*Purpose:
*  What the system hands the iOS app to open - a .wbv
*  recording from Files, Mail or another app, or a
*  winbolo:// game link - kept until the welcome screen can
*  act on it.
*
*  SDL reports both as SDL_EVENT_DROP_FILE. Every dialog,
*  the lobby and the game run their own event loops, and
*  only the welcome screen acts on a drop, so one arriving
*  anywhere else used to be polled and thrown away. An SDL
*  event watch sees each drop as it is sent, whichever loop
*  polls it afterwards, and keeps it here; the welcome
*  screen takes it the next time it runs. A cold launch
*  through a file or link is the same case: SDL sends the
*  drop on the first pump, inside the dialogs gameFrontStart
*  runs before the welcome screen.
*
*  Desktop drops are unchanged: the welcome loop still
*  handles them directly there.
*********************************************************/

#ifndef IOS_DROPS_H
#define IOS_DROPS_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*********************************************************
*NAME:          iosDropsInstall
*PURPOSE:
*  Starts watching for drops. Called once from main before
*  anything pumps events.
*
*ARGUMENTS:
*  (none)
*********************************************************/
void iosDropsInstall(void);

/*********************************************************
*NAME:          iosDropsWelcomeResult
*PURPOSE:
*  What the welcome screen should open for a kept drop, or
*  -1 for none. A game link is handed to
*  gameFrontHandleUrlOpen here and answers
*  openInternetManual, as the desktop's welcome loop does
*  for one it polls. A recording answers openLogViewer and
*  stays kept for the recordings screen to take.
*
*ARGUMENTS:
*  (none)
*********************************************************/
int iosDropsWelcomeResult(void);

/*********************************************************
*NAME:          iosDropsTakeLog
*PURPOSE:
*  Takes the kept recording's path, if there is one.
*
*ARGUMENTS:
*  out  - receives the path
*  size - size of out
*********************************************************/
bool iosDropsTakeLog(char *out, size_t size);

/*********************************************************
*NAME:          iosOpenUrlRelease
*PURPOSE:
*  Ends the access to a file opened in place from another
*  provider (iCloud Drive and the like) once it has been
*  read. Does nothing for a path that needed none. Defined
*  in ios_open_url.m, which starts that access when the
*  system hands the file over.
*
*ARGUMENTS:
*  path - the path the drop carried
*********************************************************/
void iosOpenUrlRelease(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* IOS_DROPS_H */
