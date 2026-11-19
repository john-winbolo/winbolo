/* SPDX-License-Identifier: GPL-3.0-or-later */
/*********************************************************
 *NAME:          braintest_tank_dead.h
 *PURPOSE:
 *  The one rule BrainTest uses to decide that a recorded tank is
 *  dead, so the playback view does not draw it.
 *
 *  A dead tank keeps its place on the map and its angle keeps
 *  changing for the whole respawn wait, so drawing it showed the
 *  tank turning on the spot until it respawned. The recording's
 *  deathWait is no help: the recorder builds its snapshot for one
 *  slot, and deathWait is filled for that slot only (0 for every
 *  other tank). The tankStatus death bits go to every slot, so they
 *  are the signal. This is the same rule the game client uses to
 *  stop interpolating a tank (client_snapshot.c: snap.alive).
 *********************************************************/
#ifndef BRAINTEST_TANK_DEAD_H
#define BRAINTEST_TANK_DEAD_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* True when a recorded TankSnapshot's tankStatus says the tank is in
 * its respawn wait (TANK_STATUS_DEAD) or is destroyed and not yet
 * respawned (TANK_STATUS_DESTROYED). The boat bit has no effect. */
bool braintestTankStatusIsDead(uint8_t tankStatus);

#ifdef __cplusplus
}
#endif

#endif /* BRAINTEST_TANK_DEAD_H */
