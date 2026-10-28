/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * mobile_spectator.h - A server's live delayed feed in the lobby's replay reel
 *
 * spectatorRun for the mobile clients (iOS and Android), which build the log
 * viewer's embedded reel host and not the modal viewer that hosts the feed
 * on the desktop. gamefront.c declares the call itself; this header is for
 * whatever else on mobile reaches the screen.
 */

#ifndef MOBILE_SPECTATOR_H
#define MOBILE_SPECTATOR_H

#include <stdbool.h>
#include <stdint.h>

struct SDL_Window;
struct SDL_Renderer;

#ifdef __cplusplus
extern "C" {
#endif

/* Watches the feed of the spectator ClientSim cs full screen in the reel,
   with touch pan and pinch zoom, until Close or the feed is lost. cs stays
   the caller's and is never disconnected here. Returns true when the server
   put the spectator back into its live lobby, false when the player left or
   the feed was lost. */
bool spectatorRun(struct SDL_Window *window, struct SDL_Renderer *renderer,
                  void *cs, const char *serverHost, uint16_t serverPort);

#ifdef __cplusplus
}
#endif

#endif /* MOBILE_SPECTATOR_H */
