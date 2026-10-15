/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
*Name:          iOS Recordings
*Filename:      ios_recordings.h
*Purpose:
*  The welcome loop's half of opening a .wbv the system
*  hands the app (from Files, Mail or another app). The
*  screen that lists and plays recordings is logViewerRun
*  in ios_recordings.cpp.
*********************************************************/

#ifndef IOS_RECORDINGS_H
#define IOS_RECORDINGS_H

#include <stdbool.h>
#include <SDL3/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

/*********************************************************
*NAME:          iosRecordingsTakeDropEvent
*PURPOSE:
*  Takes an SDL_EVENT_DROP_FILE that names a .wbv file and
*  keeps its path for the recordings screen to play when it
*  next opens. Returns true when the event was such a file,
*  so the caller opens that screen. Anything else, a
*  winbolo:// link included, is left alone.
*
*ARGUMENTS:
*  ev - the event polled
*********************************************************/
bool iosRecordingsTakeDropEvent(const SDL_Event *ev);

#ifdef __cplusplus
}
#endif

#endif /* IOS_RECORDINGS_H */
