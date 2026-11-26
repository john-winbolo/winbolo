/* SPDX-License-Identifier: GPL-3.0-or-later */
/*********************************************************
 *NAME:          braintest_tank_dead.c
 *PURPOSE:
 *  See braintest_tank_dead.h.
 *********************************************************/
#include "braintest_tank_dead.h"

#include "input_packet.h"

bool braintestTankStatusIsDead(uint8_t tankStatus) {
    return (tankStatus & (TANK_STATUS_DEAD | TANK_STATUS_DESTROYED)) != 0;
}
