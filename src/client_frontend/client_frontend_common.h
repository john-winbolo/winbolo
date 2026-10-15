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

#ifdef __cplusplus
}
#endif

#endif /* CLIENT_FRONTEND_COMMON_H */
