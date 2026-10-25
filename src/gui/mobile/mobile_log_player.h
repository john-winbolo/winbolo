/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * mobile_log_player.h - A downloaded game played in the lobby's replay reel
 *
 * logViewerRunFromMemory for the mobile clients (iOS and Android), which
 * build the log viewer's embedded reel host and not the modal viewer. The
 * WinBolo.net log browser's downloads and the recordings screen
 * (mobile_recordings.h) come here. gamefront.c declares the call itself;
 * this header is for the mobile screens that call it.
 */

#ifndef MOBILE_LOG_PLAYER_H
#define MOBILE_LOG_PLAYER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct SDL_Window;
struct SDL_Renderer;

#ifdef __cplusplus
extern "C" {
#endif

/* Plays the recording in zipData full screen in the reel, with pan, pinch
   zoom, play and pause and the seek bar, until Close. Owns zipData from the
   call on. A recording that will not load is reported in a message box. */
void logViewerRunFromMemory(struct SDL_Window *window,
                            struct SDL_Renderer *renderer,
                            uint8_t *zipData, size_t zipLen,
                            bool fromMainMenu);

#ifdef __cplusplus
}
#endif

#endif /* MOBILE_LOG_PLAYER_H */
