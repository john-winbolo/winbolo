/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CLIENT_FRONTEND_CONNECT_H
#define CLIENT_FRONTEND_CONNECT_H

#include <stdbool.h>

#include "client_sim.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Block until an asynchronous UDP join reaches a terminal landing, pumping
 * the transport roughly every 20ms until then. Returns true once the client
 * is playable (CLIENT_CONNECT_CONNECTED) OR has entered the server's in-game
 * lobby (clientSimIsInLobby): lobby-enabled servers replay CTRL_LOBBY_SETTINGS
 * before the map chunks, so inLobby latches true while the connect state is
 * still DOWNLOADING_MAP — a landing check that only accepts CONNECTED would
 * hang every lobby join until timeout. Returns false on timeout or a terminal
 * error; the caller reads clientSimGetConnectErrorReason for the reason.
 *
 * Shared by every SDL3 client frontend (desktop, web) so the connect/landing
 * contract has one implementation and cannot drift per platform. */
bool clientFrontAwaitJoin(ClientSim *cs, int timeoutTicks);

#ifdef __cplusplus
}
#endif

#endif /* CLIENT_FRONTEND_CONNECT_H */
