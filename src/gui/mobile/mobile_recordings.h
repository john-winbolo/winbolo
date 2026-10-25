/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * mobile_recordings.h - The recordings kept on the phone
 *
 * logViewerRun for the mobile clients (iOS and Android): with a path, the
 * recording is read and played in the reel (mobile_log_player.h); with none,
 * the list of recordings on the phone, each played when tapped, with the
 * platform's export action beside it. Where the recordings are looked for
 * and how one leaves the phone are the platform half,
 * mobile_recordings_platform.h. gamefront.c declares the call itself; this
 * header is for the mobile screens that call it.
 */

#ifndef MOBILE_RECORDINGS_H
#define MOBILE_RECORDINGS_H

#include <stdbool.h>

struct SDL_Window;
struct SDL_Renderer;

#ifdef __cplusplus
extern "C" {
#endif

/* With logPath, plays that recording in the reel; with NULL, shows the
   recordings screen until Close. A recording the system handed over
   (mobileRecordingsPlatformTakeDrop) plays first either way. */
void logViewerRun(struct SDL_Window *window, struct SDL_Renderer *renderer,
                  const char *logPath, bool fromMainMenu);

#ifdef __cplusplus
}
#endif

#endif /* MOBILE_RECORDINGS_H */
