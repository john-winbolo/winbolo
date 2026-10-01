/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CLIENT_FRONTEND_RENDER_H
#define CLIENT_FRONTEND_RENDER_H

#include <stdint.h>

#include "client_sim.h"
#include "client_net.h"     /* clientSimRenderPrepare */
#include "gfx_settings.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The per-frame step every front end runs, under the client mutex, before
 * it draws a frame: hand the sim the render clock and tell it whether the
 * Smooth animation mode is on, so other tanks are drawn from their full
 * world position rather than the game pixel. One implementation for the
 * desktop, web, Android and iOS drivers, so a step added here reaches all
 * four. */
static inline void clientFrontRenderPrepare(ClientSim *cs, uint32_t tick) {
  clientSimSetFineTankPositions(cs, gfxGetAnimSmoothness() == GFX_ANIM_SMOOTH);
  clientSimRenderPrepare(cs, tick);
}

#ifdef __cplusplus
}
#endif

#endif /* CLIENT_FRONTEND_RENDER_H */
