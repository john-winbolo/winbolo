/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CLIENT_FRONTEND_COMMON_H
#define CLIENT_FRONTEND_COMMON_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ClientSim;

/* The active-sim test every guarded frontEnd* callback makes first. The
 * ClientSim on screen is registered through frontEndSetActiveClientSim
 * (frontend.h), whose one body is in client_frontend_common.c; bot,
 * background-game and spectator sims fire the same callbacks, and theirs must
 * not write into the on-screen UI. True when cs is the registered sim, or when
 * none is registered yet (bootstrapping), so a callback goes ahead. */
bool clientFrontIsActiveUiSim(const struct ClientSim *cs);

/* Save the running game's map into dir, which is created if missing, under a
 * name made from the map's name and the time (Everard_20261016_031500.map),
 * and tell the player where it went or that it failed. For a player with no
 * file dialog: the desktop under a controller, and iOS. */
void clientFrontSaveMapAutoNamed(struct ClientSim *cs, const char *dir);

#ifdef __cplusplus
}
#endif

#endif /* CLIENT_FRONTEND_COMMON_H */
