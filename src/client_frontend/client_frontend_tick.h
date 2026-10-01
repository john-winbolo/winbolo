/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CLIENT_FRONTEND_TICK_H
#define CLIENT_FRONTEND_TICK_H

#include <stdbool.h>

#include "client_sim.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Zero the input-tick counter the keys/game cadence is derived from. Call once
 * when a game starts running so the first step is a game step on tick 0 (even
 * = game is the wire contract the server routes by; see client_frontend_tick.c
 * and server_sim.c). */
void clientFrontTickReset(void);

/* Advance the client one sim half-step and return true iff it was a full game
 * step (the caller may then run the bot brain and count the tick).
 *
 * Alternates keys/game off the input-tick counter's parity and handles the
 * lobby/countdown case (tick the transport only — no game tick while in lobby,
 * and the counter does not advance, so lobby steps cannot shift the cadence).
 * Consumes any pending gunsight adjustment into the input packet. In the keys
 * half the transport is pumped only when it does not itself advance the server
 * on tick (clientSimTransportTicksServer) — i.e. not for an active local
 * transport (wasm SP), which would double-advance the sim; UDP and
 * passive-local clients (desktop SP) pump every half-step as before.
 *
 * The platform driver owns WHEN and HOW MANY times to call this (the SDL timer
 * thread's catch-up loop vs the emscripten frame accumulator), plus suspend /
 * shutdown handling and the per-second stat rollover. Shared by the desktop and
 * web clients so the tick body has one implementation and cannot drift. */
bool clientFrontRunTickStep(ClientSim *cs);

#ifdef __cplusplus
}
#endif

#endif /* CLIENT_FRONTEND_TICK_H */
